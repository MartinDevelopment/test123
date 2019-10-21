/*
    This file is part of SAI, which is a fork of Leela Zero.
    Copyright (C) 2017-2019 Gian-Carlo Pascutto and contributors
    Copyright (C) 2018-2019 SAI Team

    SAI is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    SAI is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with SAI.  If not, see <http://www.gnu.org/licenses/>.

    Additional permission under GNU GPL version 3 section 7

    If you modify this Program, or any covered work, by linking or
    combining it with NVIDIA Corporation's libraries from the
    NVIDIA CUDA Toolkit and/or the NVIDIA CUDA Deep Neural
    Network library and/or the NVIDIA TensorRT inference library
    (or a modified version of those libraries), containing parts covered
    by the terms of the respective license agreement, the licensors of
    this Program grant you additional permission to convey the resulting
    work.
*/

#include "config.h"

#include <array>
#include <cassert>

#include "FullBoard.h"
#include "Network.h"
#include "Utils.h"
#include "Zobrist.h"

using namespace Utils;

int FullBoard::remove_string(int i) {
    int pos = i;
    int removed = 0;
    int color = m_state[i];

    do {
        m_hash    ^= Zobrist::zobrist[m_state[pos]][pos];
        m_ko_hash ^= Zobrist::zobrist[m_state[pos]][pos];

        m_state[pos] = EMPTY;
        m_parent[pos] = NUM_VERTICES;

        remove_neighbour(pos, color);

        m_empty_idx[pos]      = m_empty_cnt;
        m_empty[m_empty_cnt]  = pos;
        m_empty_cnt++;

        m_hash    ^= Zobrist::zobrist[m_state[pos]][pos];
        m_ko_hash ^= Zobrist::zobrist[m_state[pos]][pos];

        removed++;
        pos = m_next[pos];
    } while (pos != i);

    return removed;
}

std::uint64_t FullBoard::calc_ko_hash() const {
    auto res = Zobrist::zobrist_empty;

    for (auto i = 0; i < m_numvertices; i++) {
        if (m_state[i] != INVAL) {
            res ^= Zobrist::zobrist[m_state[i]][i];
        }
    }

    /* Tromp-Taylor has positional superko */
    return res;
}

template<class Function>
std::uint64_t FullBoard::calc_hash(int komove, Function transform) const {
    auto res = Zobrist::zobrist_empty;

    for (auto i = 0; i < m_numvertices; i++) {
        if (m_state[i] != INVAL) {
            res ^= Zobrist::zobrist[m_state[i]][transform(i)];
        }
    }

    /* prisoner hashing is rule set dependent */
    res ^= Zobrist::zobrist_pris[0][m_prisoners[0]];
    res ^= Zobrist::zobrist_pris[1][m_prisoners[1]];

    if (m_tomove == BLACK) {
        res ^= Zobrist::zobrist_blacktomove;
    }

    res ^= Zobrist::zobrist_ko[transform(komove)];

    return res;
}

std::uint64_t FullBoard::calc_hash(int komove) const {
    return calc_hash(komove, [](const auto vertex) { return vertex; });
}

std::uint64_t FullBoard::calc_symmetry_hash(int komove, int symmetry) const {
    return calc_hash(komove, [this, symmetry](const auto vertex) {
        if (vertex == NO_VERTEX) {
            return NO_VERTEX;
        } else {
            const auto newvtx = Network::get_symmetry(get_xy(vertex), symmetry, m_boardsize);
            return get_vertex(newvtx.first, newvtx.second);
        }
    });
}

std::uint64_t FullBoard::get_hash() const {
    return m_hash;
}

std::uint64_t FullBoard::get_ko_hash() const {
    return m_ko_hash;
}

void FullBoard::set_to_move(int tomove) {
    if (m_tomove != tomove) {
        m_hash ^= Zobrist::zobrist_blacktomove;
    }
    FastBoard::set_to_move(tomove);
}

int FullBoard::update_board(const int color, const int i) {
    assert(i != FastBoard::PASS);
    assert(m_state[i] == EMPTY);

    m_hash ^= Zobrist::zobrist[m_state[i]][i];
    m_ko_hash ^= Zobrist::zobrist[m_state[i]][i];

    m_state[i] = vertex_t(color);
    m_next[i] = i;
    m_parent[i] = i;
    m_libs[i] = count_pliberties(i);
    m_stones[i] = 1;

    m_hash ^= Zobrist::zobrist[m_state[i]][i];
    m_ko_hash ^= Zobrist::zobrist[m_state[i]][i];

    /* update neighbor liberties (they all lose 1) */
    add_neighbour(i, color);

    /* did we play into an opponent eye? */
    auto eyeplay = (m_neighbours[i] & s_eyemask[!color]);

    auto captured_stones = 0;
    int captured_vtx;

    for (int k = 0; k < 4; k++) {
        int ai = i + m_dirs[k];

        if (m_state[ai] == !color) {
            if (m_libs[m_parent[ai]] <= 0) {
                int this_captured = remove_string(ai);
                captured_vtx = ai;
                captured_stones += this_captured;
            }
        } else if (m_state[ai] == color) {
            int ip = m_parent[i];
            int aip = m_parent[ai];

            if (ip != aip) {
                if (m_stones[ip] >= m_stones[aip]) {
                    merge_strings(ip, aip);
                } else {
                    merge_strings(aip, ip);
                }
            }
        }
    }

    m_hash ^= Zobrist::zobrist_pris[color][m_prisoners[color]];
    m_prisoners[color] += captured_stones;
    m_hash ^= Zobrist::zobrist_pris[color][m_prisoners[color]];

    /* move last vertex in list to our position */
    auto lastvertex = m_empty[--m_empty_cnt];
    m_empty_idx[lastvertex] = m_empty_idx[i];
    m_empty[m_empty_idx[i]] = lastvertex;

    /* check whether we still live (i.e. detect suicide) */
    if (m_libs[m_parent[i]] == 0) {
        assert(captured_stones == 0);
        remove_string(i);
    }

    /* check for possible simple ko */
    if (captured_stones == 1 && eyeplay) {
        assert(get_state(captured_vtx) == FastBoard::EMPTY
                && !is_suicide(captured_vtx, !color));
        return captured_vtx;
    }

    // No ko
    return NO_VERTEX;
}

void FullBoard::display_board(int lastmove) const {
    FastBoard::display_board(lastmove);

    myprintf("Hash: %llX Ko-Hash: %llX\n\n", get_hash(), get_ko_hash());
}

void FullBoard::reset_board(int size) {
    FastBoard::reset_board(size);

    m_hash = calc_hash();
    m_ko_hash = calc_ko_hash();
}

bool FullBoard::remove_dead_stones(const FullBoard & tt_endboard) {
    std::vector<int> alive_stones;

#ifndef NDEBUG
    myprintf("Must Remove dead stones!\n");
    display_board();
    tt_endboard.display_board();
    myprintf ("\n   ");
    print_columns();
#endif

    for (int j = m_boardsize-1; j >= 0; j--) {
#ifndef NDEBUG
        myprintf("%2d ", j+1);
#endif
        for (int i = 0; i < m_boardsize; i++) {
            int vertex = get_vertex(i, j);
            const auto currcolor = get_state(vertex);
            const auto endcolor = tt_endboard.get_state(vertex);
            if (currcolor == EMPTY) {
#ifndef NDEBUG
                myprintf (". ");
#endif
            } else if (currcolor == endcolor) {
                alive_stones.push_back(vertex);
#ifndef NDEBUG
                myprintf ("A ");
#endif
            } else {
#ifndef NDEBUG
                myprintf ("v ");
#endif
            }
        }
#ifndef NDEBUG
        myprintf("%2d\n", j+1);
#endif
    }
#ifndef NDEBUG
    myprintf("   ");
    print_columns();
    myprintf("\n");
#endif


    for (int i = 0; i < m_boardsize; i++) {
        for (int j = 0; j < m_boardsize; j++) {
            int vertex = get_vertex(i, j);
            const auto currcolor = get_state(vertex);
            const auto endcolor = tt_endboard.get_state(vertex);
            if (currcolor != EMPTY && currcolor != endcolor) {
                m_prisoners[!currcolor] += remove_string(vertex);
            }
        }
    }
    for (auto vertex : alive_stones) {
        if (get_state(vertex) == EMPTY) {
            return false;
        }
    }
    return true;
}

