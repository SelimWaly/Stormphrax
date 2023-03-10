/*
 * Polaris, a UCI chess engine
 * Copyright (C) 2023 Ciekce
 *
 * Polaris is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Polaris is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Polaris. If not, see <https://www.gnu.org/licenses/>.
 */

#include "perft.h"

#include <iostream>

#include "movegen.h"
#include "uci.h"

namespace polaris
{
	namespace
	{
		size_t doPerft(Position &pos, i32 depth)
		{
			if (depth == 0)
				return 1;

			--depth;

			const auto opp = pos.opponent();

			MoveList moves{};
			MoveGenerator generator{pos, moves, Move::null()};

			size_t total{};

			while (const auto move = generator.next())
			{
				const auto guard = pos.applyMove<false>(move);

				if (pos.isAttacked(opp == Color::Black ? pos.whiteKing() : pos.blackKing(), opp))
					continue;

				total += doPerft(pos, depth);
			}

			return total;
		}
	}

	void perft(Position &pos, i32 depth)
	{
		--depth;

		const auto opp = pos.opponent();

		MoveList moves{};
		MoveGenerator generator{pos, moves, Move::null()};

		size_t total{};

		while (const auto move = generator.next())
		{
			const auto guard = pos.applyMove<false>(move);

			if (pos.isAttacked(opp == Color::Black ? pos.whiteKing() : pos.blackKing(), opp))
				continue;

			const auto value = doPerft(pos, depth);

			total += value;
			std::cout << uci::moveToString(move) << '\t' << value << '\n';
		}

		std::cout << '\n' << "total " << total << std::endl;
	}
}
