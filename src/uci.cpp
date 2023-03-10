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

#include "uci.h"

#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <iomanip>

#include "util/split.h"
#include "util/parse.h"
#include "position.h"
#include "search/search.h"
#include "movegen.h"
#include "eval/eval.h"
#include "pretty.h"
#include "ttable.h"
#include "limit/trivial.h"
#include "limit/time.h"
#include "perft.h"

#include "hash.h"
#include "eval/material.h"

namespace polaris::uci
{
	namespace
	{
		constexpr auto Name = "Polaris";
		constexpr auto Version = PS_STRINGIFY(PS_VERSION);
		constexpr auto Author = "Ciekce";

		template <typename T>
		constexpr T ceilDiv(T n, T d)
		{
			return (n + d - 1) / d;
		}

#if 0
		void orderMoves(MoveList &moves, const Position &pos)
		{
			std::stable_sort(moves.begin(), moves.end(), [&pos](const auto a, const auto b)
			{
				const auto aSrc = a.src();
				const auto bSrc = b.src();

				const auto aDst = a.dst();
				const auto bDst = b.dst();

				const auto aDstPiece = pos.pieceAt(aDst);
				const auto bDstPiece = pos.pieceAt(bDst);

				const bool aCapture = aDstPiece != Piece::None;
				const bool bCapture = bDstPiece != Piece::None;

				if (aCapture && bCapture)
				{
					const auto aSrcPiece = pos.pieceAt(aSrc);
					const auto bSrcPiece = pos.pieceAt(bSrc);

					const auto aCapturedValue = eval::pieceValue(aDstPiece).midgame;
					const auto bCapturedValue = eval::pieceValue(bDstPiece).midgame;

					const auto aDelta = aCapturedValue - eval::pieceValue(aSrcPiece).midgame;
					const auto bDelta = bCapturedValue - eval::pieceValue(bSrcPiece).midgame;

					if ((aDelta == bDelta && aCapturedValue > bCapturedValue)
						|| aDelta > bDelta)
						return true;
					else if (aDelta < bDelta)
						return false;
				}
				else if (aCapture)
					return true;

				const auto aFileDist = 3 - (squareFile(aDst) > 3 ? 7 - squareFile(aDst) : squareFile(aDst));
				const auto bFileDist = 3 - (squareFile(bDst) > 3 ? 7 - squareFile(bDst) : squareFile(bDst));

				if (aFileDist < bFileDist)
					return true;
				else if (aFileDist > bFileDist)
					return false;

				return false;
			});
		}
#endif
	}

	i32 run()
	{
		auto searcher = search::ISearcher::createDefault();
		auto pos = Position::starting();

		std::optional<size_t> hashSize{};
		i32 moveOverhead = limit::DefaultMoveOverhead;

		for (std::string line{}; std::getline(std::cin, line);)
		{
			const auto tokens = split::split(line, ' ');

			if (tokens.empty())
				continue;

			const auto &command = tokens[0];

			if (command == "quit")
				return 0;
			else if (command == "uci")
			{
				std::cout << "id name " << Name << ' ' << Version << '\n';
				std::cout << "id author " << Author << '\n';

				std::cout << "option name Hash type spin default " << DefaultHashSize
					<< " min " << HashSizeRange.min() << " max " << HashSizeRange.max() << '\n';
				std::cout << "option name Clear Hash type button\n";
			//	std::cout << "option name Ponder type check default false\n";
				std::cout << "option name Contempt type spin default 0 min -10000 max 10000\n";
			//	std::cout << "option name Searcher type combo default AspPVS var AlphaBeta var AspPVS var MTDf var MCTS\n";
			//	std::cout << "option name Underpromotions type check default false\n";
				std::cout << "option name Move Overhead type spin default " << limit::DefaultMoveOverhead
					<< " min " << limit::MoveOverheadRange.min() << " max " << limit::MoveOverheadRange.max() << '\n';

				std::cout << "option name Searcher type combo default AspPVS";

				for (const auto &searcherName : search::ISearcher::searchers())
				{
					std::cout << " var " << searcherName;
				}

				std::cout << '\n';

				std::cout << "uciok" << std::endl;
			}
			else if (command == "ucinewgame")
				searcher->newGame();
			else if (command == "isready")
				std::cout << "readyok" << std::endl;
			else if (command == "position")
			{
				if (tokens.size() > 1)
				{
					const auto &position = tokens[1];

					size_t next = 2;

					if (position == "startpos")
						pos = Position::starting();
					else if (position == "fen")
					{
						std::ostringstream fen{};

						for (size_t i = 0; i < 6 && next < tokens.size(); ++i, ++next)
						{
							fen << tokens[next] << ' ';
						}

						if (auto newPos = Position::fromFen(fen.str()))
							pos = std::move(*newPos);
						else
						{
							std::cerr << "invalid fen" << std::endl;
							continue;
						}
					}
					else continue;

					if (next < tokens.size() && tokens[next++] == "moves")
					{
						for (; next < tokens.size(); ++next)
						{
							if (const auto move = pos.moveFromUci(tokens[next]))
								pos.applyMoveUnchecked<false>(move);
						}

						pos.regenMaterial();
					}
				}
			}
			else if (command == "go")
			{
				u32 depth = search::MaxDepth;
				std::unique_ptr<limit::ISearchLimiter> limiter{};

				bool tournamentTime = false;

				const auto startTime = util::g_timer.time();

				i64 timeRemaining{};
				i64 increment{};
				i32 toGo{};

				for (size_t i = 1; i < tokens.size(); ++i)
				{
					if (tokens[i] == "depth" && ++i < tokens.size())
					{
						if (!util::tryParseU32(depth, tokens[i]))
							std::cerr << "invalid depth " << tokens[i] << std::endl;
					}
					else if (!tournamentTime && !limiter)
					{
						if (tokens[i] == "infinite")
							limiter = std::make_unique<limit::InfiniteLimiter>();
						else if (tokens[i] == "nodes" && ++i < tokens.size())
						{
							std::cout << "info string node limiting currently broken" << std::endl;

							size_t nodes{};
							if (!util::tryParseSize(nodes, tokens[i]))
								std::cerr << "invalid node count " << tokens[i] << std::endl;
							else limiter = std::make_unique<limit::NodeLimiter>(nodes);
						}
						else if (tokens[i] == "movetime" && ++i < tokens.size())
						{
							u64 time{};
							if (!util::tryParseU64(time, tokens[i]))
								std::cerr << "invalid time " << tokens[i] << std::endl;
							else
							{
								time = std::clamp<u64>(time, 1, static_cast<u64>(std::numeric_limits<i64>::max()));
								limiter = std::make_unique<limit::MoveTimeLimiter>(static_cast<i64>(time), moveOverhead);
							}
						}
						else if ((tokens[i] == "btime" || tokens[i] == "wtime") && ++i < tokens.size()
							&& tokens[i - 1] == (pos.toMove() == Color::Black ? "btime" : "wtime"))
						{
							tournamentTime = true;

							u64 time{};
							if (!util::tryParseU64(time, tokens[i]))
								std::cerr << "invalid time " << tokens[i] << std::endl;
							else
							{
								time = std::clamp<u64>(time, 1, static_cast<u64>(std::numeric_limits<i64>::max()));
								timeRemaining = static_cast<i64>(time);
							}
						}
						else if ((tokens[i] == "binc" || tokens[i] == "winc") && ++i < tokens.size()
							&& tokens[i - 1] == (pos.toMove() == Color::Black ? "binc" : "winc"))
						{
							tournamentTime = true;

							u64 time{};
							if (!util::tryParseU64(time, tokens[i]))
								std::cerr << "invalid time " << tokens[i] << std::endl;
							else
							{
								time = std::clamp<u64>(time, 1, static_cast<u64>(std::numeric_limits<i64>::max()));
								increment = static_cast<i64>(time);
							}
						}
						else if (tokens[i] == "movestogo" && ++i < tokens.size())
						{
							tournamentTime = true;

							u32 moves{};
							if (!util::tryParseU32(moves, tokens[i]))
								std::cerr << "invalid movestogo " << tokens[i] << std::endl;
							else
							{
								moves = std::min<u32>(moves, static_cast<u32>(std::numeric_limits<i32>::max()));
								toGo = static_cast<i32>(moves);
							}
						}
					}
					// yeah I hate the duplication too
					else if (tournamentTime)
					{
						if ((tokens[i] == "btime" || tokens[i] == "wtime") && ++i < tokens.size()
							&& tokens[i - 1] == (pos.toMove() == Color::Black ? "btime" : "wtime"))
						{
							u64 time{};
							if (!util::tryParseU64(time, tokens[i]))
								std::cerr << "invalid time " << tokens[i] << std::endl;
							else
							{
								time = std::clamp<u64>(time, 1, static_cast<u64>(std::numeric_limits<i64>::max()));
								timeRemaining = static_cast<i64>(time);
							}
						}
						else if ((tokens[i] == "binc" || tokens[i] == "winc") && ++i < tokens.size()
							&& tokens[i - 1] == (pos.toMove() == Color::Black ? "binc" : "winc"))
						{
							u64 time{};
							if (!util::tryParseU64(time, tokens[i]))
								std::cerr << "invalid time " << tokens[i] << std::endl;
							else
							{
								time = std::clamp<u64>(time, 1, static_cast<u64>(std::numeric_limits<i64>::max()));
								increment = static_cast<i64>(time);
							}
						}
						else if (tokens[i] == "movestogo" && ++i < tokens.size())
						{
							u32 moves{};
							if (!util::tryParseU32(moves, tokens[i]))
								std::cerr << "invalid movestogo " << tokens[i] << std::endl;
							else
							{
								moves = std::min<u32>(moves, static_cast<u32>(std::numeric_limits<i32>::max()));
								toGo = static_cast<i32>(moves);
							}
						}
					}
				}

				if (depth > search::MaxDepth)
					depth = search::MaxDepth;

				if (tournamentTime && timeRemaining > 0)
					limiter = std::make_unique<limit::TimeManager>(startTime,
						static_cast<f64>(timeRemaining) / 1000.0,
						static_cast<f64>(increment) / 1000.0,
						toGo, static_cast<f64>(moveOverhead) / 1000.0);
				else if (!limiter)
					limiter = std::make_unique<limit::InfiniteLimiter>();

				searcher->startSearch(pos, static_cast<i32>(depth), std::move(limiter));
			}
			else if (command == "stop")
				searcher->stop();
			else if (command == "setoption")
			{
				size_t i = 1;

				for (; i < tokens.size() - 1 && tokens[i] != "name"; ++i) {}

				if (++i == tokens.size())
					continue;

				bool nameEmpty = true;
				std::ostringstream name{};

				for (; i < tokens.size() && tokens[i] != "value"; ++i)
				{
					if (!nameEmpty)
						name << ' ';
					else nameEmpty = false;

					name << tokens[i];
				}

				if (++i == tokens.size())
					continue;

				bool valueEmpty = true;
				std::ostringstream value{};

				for (; i < tokens.size(); ++i)
				{
					if (!valueEmpty)
						value << ' ';
					else valueEmpty = false;

					value << tokens[i];
				}

				auto nameStr = name.str();

				const auto valueStr = value.str();

				if (!nameEmpty)
				{
					std::transform(nameStr.begin(), nameStr.end(), nameStr.begin(),
						[](auto c) { return std::tolower(c); });

					if (nameStr == "hash")
					{
						if (!valueEmpty)
						{
							if (const auto newHashSize = util::tryParseSize(valueStr))
							{
								hashSize = HashSizeRange.clamp(*newHashSize);
								searcher->setHashSize(*hashSize);
							}
						}
					}
					else if (nameStr == "clear hash")
						searcher->clearHash();
					else if (nameStr == "searcher")
					{
						if (!valueEmpty)
						{
							if (auto newSearcher = search::ISearcher::create(valueStr, hashSize))
								searcher = std::move(*newSearcher);
							else std::cerr << "unknown searcher " << valueStr << std::endl;
						}
					}
					if (nameStr == "hash")
					{
						if (!valueEmpty)
						{
							if (const auto newMoveOverhead = util::tryParseI32(valueStr))
								moveOverhead = limit::MoveOverheadRange.clamp(*newMoveOverhead);
						}
					}
				}
			}
			// V ======= NONSTANDARD ======= V
			else if (command == "d")
			{
				std::cout << '\n';

				printBoard(std::cout, pos);
				std::cout << "\nFen: " << pos.toFen() << std::endl;

				std::ostringstream key{};
				key << std::hex << std::setw(16) << std::setfill('0') << pos.key();
				std::cout << "Key: " << key.view() << std::endl;

				std::ostringstream pawnKey{};
				pawnKey << std::hex << std::setw(16) << std::setfill('0') << pos.pawnKey();
				std::cout << "Pawn key: " << pawnKey.view() << std::endl;

				std::cout << "Checkers:";

				auto checkers = pos.checkers();
				while (checkers)
				{
					std::cout << ' ' << squareToString(checkers.popLowestSquare());
				}

				std::cout << std::endl;

				const auto staticEval = eval::staticEvalAbs(pos);
				std::cout << "Static eval: ";
				printScore(std::cout, staticEval);
				std::cout << std::endl;
			}
			else if (command == "eval")
			{
			//	const auto staticEval = eval::staticEvalAbs(pos);
			//	printScore(std::cout, staticEval);
			//	std::cout << std::endl;
			//	std::cout << (staticEval > 0 ? "+" : "") << (static_cast<f64>(staticEval) / 100.0) << std::endl;
				eval::printEval(pos);
			}
			else if (command == "checkers")
			{
				std::cout << '\n';
				printBitboard(std::cout, pos.checkers());
			}
			else if (command == "regen")
				pos.regen();
			else if (command == "moves")
			{
				MoveList moves{};
				generateAll(moves, pos);

				for (u32 i = 0; i < moves.size(); ++i)
				{
					if (i > 0)
						std::cout << ' ';
					std::cout << moveToString(moves[i]);
				}

				std::cout << std::endl;
			}
			else if (command == "perft")
			{
				u32 depth = 6;

				for (size_t i = 1; i < tokens.size(); ++i)
				{
					if (tokens[i] == "depth" && ++i < tokens.size())
					{
						if (!util::tryParseU32(depth, tokens[i]))
							std::cerr << "invalid depth " << tokens[i] << std::endl;
					}
				}

				perft(pos, static_cast<i32>(depth));
			}
#ifndef NDEBUG
			else if (command == "verify")
			{
				if (pos.verify())
					std::cout << "info string boards and keys ok" << std::endl;
			}
#endif
		}

		return 0;
	}

	/*
	void printInfoNoPv(const SearchData &stats, u32 depth, Score score, Color toMove, f64 hashFilled)
	{
		std::cout << "info";

		const auto totalNodes = stats.standardNodes + stats.quiescenceNodes;

		std::cout << " depth " << depth;
		std::cout << " seldepth " << stats.seldepth;
		std::cout << " nodes " << totalNodes;
		std::cout << " nps " << static_cast<size_t>(std::lround(static_cast<f64>(totalNodes) / stats.time));

		std::cout << " score ";

		if (std::abs(score) > 500000)
			std::cout << "mate " << (static_cast<i32>(Mate - std::abs(score)) / 2 * static_cast<i32>(toMove));
		else std::cout << "cp " << score;

		std::cout << " hashfull " << static_cast<i32>(std::round(hashFilled * 1000));
	}
	 */

	std::string moveToString(Move move)
	{
		if (!move)
			return "0000";

		std::ostringstream str{};

		str << squareToString(move.src()) << squareToString(move.dst());

		if (move.type() == MoveType::Promotion)
			str << basePieceToChar(move.target());

		return str.str();
	}

#ifndef NDEBUG
	std::string moveAndTypeToString(Move move)
	{
		if (!move)
			return "0000";

		std::ostringstream str{};

		if (move.type() != MoveType::Standard)
		{
			switch (move.type())
			{
			case MoveType::Promotion: str << "p:"; break;
			case MoveType::Castling: str << "c:"; break;
			case MoveType::EnPassant: str << "e:"; break;
			default: __builtin_unreachable();
			}
		}

		str << moveToString(move);

		return str.str();
	}
#endif
}
