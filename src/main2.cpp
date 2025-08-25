#include "boost/beast/core.hpp"
#include "boost/beast/websocket.hpp"
#include "boost/asio/ip/tcp.hpp"
#include "boost/beast/http.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <algorithm>
#include <memory>

#include "chess.hpp"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;

// Deklarasi Awal untuk fungsi global
void broadcast(const std::string& message);
void broadcast_player_list();
void broadcast_board_state();

// ============== State Catur & Pemain Global ============== 

class PlayerSession;

chess::Board board;
std::mutex board_mutex;

std::vector<std::shared_ptr<PlayerSession>> players;
std::mutex players_mutex;

enum class GameState {
    WAITING_FOR_PLAYERS,
    READY_TO_START,
    IN_PROGRESS,
    GAME_OVER
};

GameState current_game_state = GameState::WAITING_FOR_PLAYERS;
std::mutex game_state_mutex;

// Kelas untuk merepresentasikan sesi seorang pemain
class PlayerSession : public std::enable_shared_from_this<PlayerSession> {
public:
    websocket::stream<tcp::socket> ws;
    std::string username;
    chess::Color color = chess::Color::NONE;

    PlayerSession(tcp::socket socket) : ws(std::move(socket)) {}

    void start() {
        try {
            ws.set_option(websocket::stream_base::decorator(
                [](websocket::response_type& res) {
                    res.set(http::field::server, "fchess_server-beast");
                }));
            ws.accept();
            
            beast::flat_buffer buffer;
            ws.read(buffer);
            std::string login_message = beast::buffers_to_string(buffer.data());

            if (login_message.rfind("LOGIN:", 0) == 0) {
                this->username = login_message.substr(6);
            } else {
                return;
            }

            {
                std::lock_guard<std::mutex> lock(players_mutex);
                bool white_taken = false;
                for(const auto& p : players) if (p->color == chess::Color::WHITE) white_taken = true;
                if (!white_taken) {
                    this->color = chess::Color::WHITE;
                } else {
                    bool black_taken = false;
                    for(const auto& p : players) if (p->color == chess::Color::BLACK) black_taken = true;
                    if (!black_taken) this->color = chess::Color::BLACK;
                    else this->color = chess::Color::NONE;
                }
                players.push_back(shared_from_this());
            }

            // Perbarui status game jika ada 2 pemain
            {
                std::lock_guard<std::mutex> lock(game_state_mutex);
                if (current_game_state == GameState::WAITING_FOR_PLAYERS && players.size() >= 2) {
                    bool has_white = false;
                    bool has_black = false;
                    for(const auto& p : players) {
                        if (p->color == chess::Color::WHITE) has_white = true;
                        if (p->color == chess::Color::BLACK) has_black = true;
                    }
                    if (has_white && has_black) {
                        current_game_state = GameState::READY_TO_START;
                    }
                }
            }

            send_current_board_state();
            broadcast_player_list();
            broadcast_game_state(); // Broadcast status game baru

            read_loop();

        } catch (beast::system_error const& se) {
            if (se.code() != websocket::error::closed) std::cerr << "Session start error: " << se.code().message() << std::endl;
        } catch (std::exception const& e) {
            std::cerr << "Session start exception: " << e.what() << std::endl;
        }
    }

private:
    void read_loop() {
        try {
            for (;;) {
                beast::flat_buffer buffer;
                ws.read(buffer);
                handle_message(beast::buffers_to_string(buffer.data()));
            }
        } catch (beast::system_error const& se) {
            if (se.code() != websocket::error::closed) std::cerr << "Read loop error: " << se.code().message() << std::endl;
        } catch (std::exception const& e) {
            std::cerr << "Read loop exception: " << e.what() << std::endl;
        }
        cleanup();
    }

    void handle_message(const std::string& message_str) {
        std::lock_guard<std::mutex> lock(board_mutex);

        // Handle START_GAME command
        if (message_str == "START_GAME") {
            std::lock_guard<std::mutex> gs_lock(game_state_mutex);
            if (current_game_state == GameState::READY_TO_START) {
                current_game_state = GameState::IN_PROGRESS;
                broadcast_game_state();
                send_message("INFO:Game started!");
            } else {
                send_message("ERROR:Game cannot be started now.");
            }
            return;
        }

        // Hanya izinkan gerakan jika game sedang berjalan
        {
            std::lock_guard<std::mutex> gs_lock(game_state_mutex);
            if (current_game_state != GameState::IN_PROGRESS) {
                send_message("ERROR:Game is not in progress. Cannot make moves.");
                return;
            }
        }

        // Hanya pemain (bukan penonton) yang bisa bergerak
        if (this->color == chess::Color::NONE) {
            send_message("ERROR:Spectators cannot make moves.");
            return;
        }

        // Cek apakah giliran pemain ini
        if (board.sideToMove() != this->color) {
            send_message("ERROR:It's not your turn.");
            return;
        }

        chess::Move move;
        if (!parseUci(message_str, move)) {
            send_message("ERROR:Invalid move format (e.g., e2e4).");
            return;
        }

        chess::Movelist legal_moves;
        chess::movegen::legalmoves(legal_moves, board);
        bool move_is_legal = std::find(legal_moves.begin(), legal_moves.end(), move) != legal_moves.end();

        if (move_is_legal) {
            board.makeMove(move);
            std::string fen_msg = "FEN:" + board.getFen();
            auto game_over_info = board.isGameOver();
            if (game_over_info.first !=chess::GameResultReason::NONE) {
                fen_msg += "|GAMEOVER:";
                auto reason = game_over_info.first;
                if (reason == chess::GameResultReason::CHECKMATE) {
                    fen_msg += "Checkmate!";
                    current_game_state = GameState::GAME_OVER;
                } else if (reason == chess::GameResultReason::STALEMATE) {
                    fen_msg += "Draw by Stalemate.";
                    current_game_state = GameState::GAME_OVER;
                } else {
                    fen_msg += "Game Over.";
                    current_game_state = GameState::GAME_OVER;
                }
                broadcast_game_state(); // Broadcast game over state
            }
            broadcast(fen_msg);
        } else {
            send_message("ERROR:Illegal move.");
        }
    }

    void send_message(const std::string& message) {
        try {
            ws.text(true);
            ws.write(net::buffer(message));
        } catch (...) { /* ignore */ }
    }

    void send_current_board_state() {
        std::lock_guard<std::mutex> lock(board_mutex);
        send_message("FEN:" + board.getFen());
    }

    void cleanup() {
        std::lock_guard<std::mutex> lock(players_mutex);
        players.erase(std::remove_if(players.begin(), players.end(),
            [this](const std::shared_ptr<PlayerSession>& p) { return p.get() == this; }), players.end());
        
        // Reset game state if not enough players
        {
            std::lock_guard<std::mutex> gs_lock(game_state_mutex);
            if (players.size() < 2 && current_game_state != GameState::WAITING_FOR_PLAYERS) {
                current_game_state = GameState::WAITING_FOR_PLAYERS;
                // Reset board if game was in progress
                if (current_game_state == GameState::IN_PROGRESS || current_game_state == GameState::GAME_OVER) {
                    board = chess::Board(); // Reset to start position
                }
            }
        }

        broadcast_player_list();
        broadcast_game_state();
    }

    bool parseUci(const std::string& uci_str, chess::Move& move) {
        if (uci_str.length() < 4 || uci_str.length() > 5) return false;
        try {
            auto from_sq = chess::Square(uci_str.substr(0, 2));
            auto to_sq = chess::Square(uci_str.substr(2, 2));
            if (uci_str.length() == 5) {
                auto promotion = chess::PieceType(uci_str.substr(4, 1));
                move = chess::Move::make<chess::Move::PROMOTION>(from_sq, to_sq, promotion);
            } else {
                move = chess::Move::make(from_sq, to_sq);
            }
            return true;
        } catch (...) {
            return false;
        }
    }
};

void broadcast(const std::string& message) {
    std::vector<std::shared_ptr<PlayerSession>> players_copy;
    {
        std::lock_guard<std::mutex> lock(players_mutex);
        players_copy = players;
    }
    for (const auto& p : players_copy) {
        try {
            p->ws.text(true);
            p->ws.write(net::buffer(message));
        } catch (...) { /* ignore */ }
    }
}

void broadcast_player_list() {
    std::string player_list_msg = "PLAYERS:";
    {
        std::lock_guard<std::mutex> lock(players_mutex);
        for (const auto& p : players) {
            player_list_msg += p->username + "(" + (p->color == chess::Color::WHITE ? "W" : p->color == chess::Color::BLACK ? "B" : "S") + ");";
        }
    }
    broadcast(player_list_msg);
}

void broadcast_game_state() {
    std::string game_state_msg = "GAME_STATE:";
    {
        std::lock_guard<std::mutex> lock(game_state_mutex);
        switch (current_game_state) {
            case GameState::WAITING_FOR_PLAYERS:
                game_state_msg += "WAITING_FOR_PLAYERS";
                break;
            case GameState::READY_TO_START:
                game_state_msg += "READY_TO_START";
                break;
            case GameState::IN_PROGRESS:
                game_state_msg += "IN_PROGRESS";
                break;
            case GameState::GAME_OVER:
                game_state_msg += "GAME_OVER";
                break;
        }
    }
    broadcast(game_state_msg);
}

int main(int argc, char* argv[]) {
    chess::attacks::initAttacks();

    auto const address = net::ip::make_address("0.0.0.0");
    auto const port = static_cast<unsigned short>(8080);

    net::io_context ioc;
    tcp::acceptor acceptor(ioc, {address, port});
    std::cout << "Chess server with usernames started on 0.0.0.0:" << port << std::endl;

    for (;;)
    {
        tcp::socket socket(ioc);
        acceptor.accept(socket);
        std::thread(&PlayerSession::start, std::make_shared<PlayerSession>(std::move(socket))).detach();
    }

    return EXIT_SUCCESS;
}