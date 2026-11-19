#include "minesweeper.h"

#include <algorithm>
#include <cstdlib>
#include <random>

namespace {
template<typename Visit>
void each_neighbor(int index, Visit visit)
{
    constexpr int side = Minesweeper::side;
    int x = index % side, y = index / side;
    for (int row = std::max(0, y - 1); row <= std::min(side - 1, y + 1); ++row)
        for (int col = std::max(0, x - 1); col <= std::min(side - 1, x + 1); ++col)
            if (row != y || col != x) visit(row * side + col);
}
}

void Minesweeper::plant(int first, unsigned seed)
{
    std::array<int, cells> candidates;
    int count = 0;
    for (int i = 0; i < cells; ++i) {
        if (std::abs(i / side - first / side) <= 1 && std::abs(i % side - first % side) <= 1)
            continue;
        candidates[count++] = i;
    }
    std::minstd_rand random(seed);
    std::shuffle(candidates.begin(), candidates.begin() + count, random);
    for (int n = 0; n < mines; ++n) {
        int index = candidates[n];
        board[index].mine = true;
        each_neighbor(index, [&](int i) { ++board[i].neighbors; });
    }
    state = Phase::playing;
}

void Minesweeper::toggle_flag(int index)
{
    auto &cell = board[index];
    if (state == Phase::won || state == Phase::lost || cell.open) return;
    if (!cell.flag && flags == mines) return;
    cell.flag = !cell.flag;
    flags += cell.flag ? 1 : -1;
}

void Minesweeper::reveal(int index, unsigned seed)
{
    auto &cell = board[index];
    if (state == Phase::won || state == Phase::lost || cell.open || cell.flag) return;
    if (state == Phase::ready) plant(index, seed);
    cell.open = true;
    if (cell.mine) {
        state = Phase::lost;
        return;
    }

    std::array<int, cells> pending;
    int count = 0;
    pending[count++] = index;
    while (count) {
        int current = pending[--count];
        ++opened;
        if (board[current].neighbors) continue;
        each_neighbor(current, [&](int i) {
            if (board[i].open || board[i].flag) return;
            board[i].open = true;
            pending[count++] = i;
        });
    }
    if (opened == cells - mines) {
        state = Phase::won;
        for (auto &tile : board) tile.flag = tile.mine;
        flags = mines;
    }
}
