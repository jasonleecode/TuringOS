#pragma once

#include <array>

class Minesweeper
{
public:
    static constexpr int side = 9, cells = side * side, mines = 10;
    struct Cell {
        bool mine = false, open = false, flag = false;
        unsigned neighbors = 0;
    };
    enum class Phase { ready, playing, won, lost };

    const Cell &at(int index) const { return board[index]; }
    Phase phase() const { return state; }
    int remaining_flags() const { return mines - flags; }
    void toggle_flag(int index);
    void reveal(int index, unsigned seed);

private:
    void plant(int first, unsigned seed);
    std::array<Cell, cells> board{};
    Phase state = Phase::ready;
    int flags = 0, opened = 0;
};
