// Run on the host with the command in ../README.md.
#include "../src/minesweeper.h"
#include <cassert>
#include <cstdlib>

using Phase = Minesweeper::Phase;

int main()
{
    Minesweeper flags;
    for (int i = 0; i <= flags.mines; ++i) flags.toggle_flag(i);
    assert(flags.remaining_flags() == 0 && !flags.at(flags.mines).flag);
    flags.reveal(0, 1);
    assert(flags.phase() == Phase::ready);
    flags.toggle_flag(0);
    flags.reveal(0, 1);
    assert(flags.at(0).open && flags.at(1).flag && !flags.at(1).open);

    // Check every opening position, including edges and corners.
    for (int first = 0; first < Minesweeper::cells; ++first) {
        Minesweeper game;
        game.reveal(first, 42 + first);
        int mines = 0, last_mine = -1;
        for (int i = 0; i < game.cells; ++i) {
            int adjacent = 0;
            for (int j = 0; j < game.cells; ++j) {
                if (i == j || std::abs(i % game.side - j % game.side) > 1 ||
                    std::abs(i / game.side - j / game.side) > 1) continue;
                adjacent += game.at(j).mine;
                if (i == first) assert(game.at(j).open);
            }
            assert(adjacent == static_cast<int>(game.at(i).neighbors));
            if (game.at(i).mine) {
                assert(!game.at(i).open);
                ++mines;
                last_mine = i;
            }
        }
        assert(mines == game.mines && game.at(first).open && game.at(first).neighbors == 0);
        assert(game.phase() == Phase::playing);
        auto lost = game;
        lost.reveal(last_mine, 0);
        lost.toggle_flag(last_mine);
        assert(lost.phase() == Phase::lost && lost.at(last_mine).open && !lost.at(last_mine).flag);
        for (int i = 0; i < game.cells; ++i)
            if (!game.at(i).mine) game.reveal(i, 0);
        assert(game.phase() == Phase::won && game.remaining_flags() == 0);
        game.reveal(last_mine, 0);
        game.toggle_flag(last_mine);
        assert(game.at(last_mine).flag && !game.at(last_mine).open);
    }
}
