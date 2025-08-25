#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <queue>
#include <cmath> // Diperlukan untuk fungsi pow()
#undef from
#undef to
#include "chess.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = boost::beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;
using namespace chess;

// Fungsi untuk menghitung perubahan rating Elo
void calculateNewRatings(int& playerRating, int& opponentRating, double actualScore) {
    // Untuk sementara kita gunakan K-factor tetap
    const int K = 32;

    // 1. Hitung skor yang diharapkan
    double expectedScore = 1.0 / (1.0 + pow(10.0, (double)(opponentRating - playerRating) / 400.0));

    // 2. Hitung rating baru
    playerRating = (int)round(playerRating + K * (actualScore - expectedScore));
}

class ChessGame {
private:
    chess::Board board_;
    bool gameActive;
    std::string gameResult;
    long long whiteTimeRemainingMs_;
    long long blackTimeRemainingMs_;
    std::chrono::steady_clock::time_point lastMoveTime_;
    long long initialTimeMs_;
    long long incrementMs_;
    bool isAiGame_ = false;
    std::string whitePlayerName_ = "White";
    int whitePlayerRating_ = 0;
    std::string blackPlayerName_ = "Black";
    int blackPlayerRating_ = 0;

public:
    ChessGame(long long initialTimeMs, long long incrementMs) : initialTimeMs_(initialTimeMs), incrementMs_(incrementMs) {
        resetBoard();
    }
    long long getInitialTimeMs() const { return initialTimeMs_; }
    long long getIncrementMs() const { return incrementMs_; }
    // --- Fungsi yang Dipertahankan (Tidak Berubah) ---
    void setAsAiGame(bool isAi) { isAiGame_ = isAi; }
    bool isAiGame() const { return isAiGame_; }
    bool isActive() const { return gameActive; }
    void setPlayerNames(const std::string& white, const std::string& black) {
        whitePlayerName_ = white;
        blackPlayerName_ = black;
    }
    void setPlayerNamesAndRatings(const std::string& whiteName, int whiteRating, const std::string& blackName, int blackRating) {
        whitePlayerName_ = whiteName;
        whitePlayerRating_ = whiteRating;
        blackPlayerName_ = blackName;
        blackPlayerRating_ = blackRating;
    }
    void endGame(const std::string& winner, const std::string& reason) {
        gameActive = false;
        gameResult = winner + " wins by " + reason;
    }
    void updateTime() {
        auto now = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastMoveTime_).count();
        lastMoveTime_ = now;
        if (getCurrentPlayer() == "white") {
            whiteTimeRemainingMs_ -= elapsedMs;
            if (whiteTimeRemainingMs_ < 0) whiteTimeRemainingMs_ = 0;
        }
        else {
            blackTimeRemainingMs_ -= elapsedMs;
            if (blackTimeRemainingMs_ < 0) blackTimeRemainingMs_ = 0;
        }
    }
    bool checkTimeout() const {
        if (getCurrentPlayer() == "white") return whiteTimeRemainingMs_ <= 0;
        else return blackTimeRemainingMs_ <= 0;
    }
    long long getWhiteTimeRemaining() const { return whiteTimeRemainingMs_; }
    long long getBlackTimeRemaining() const { return blackTimeRemainingMs_; }

    // --- Fungsi yang Berubah (Sudah Diperbaiki) ---

    void resetBoard() {
        // FIX: Menggunakan metode setFen (camelCase) dan konstanta STARTPOS
        board_.setFen(chess::constants::STARTPOS);
        gameActive = true;
        gameResult = "";
        whiteTimeRemainingMs_ = initialTimeMs_;
        blackTimeRemainingMs_ = initialTimeMs_;
        lastMoveTime_ = std::chrono::steady_clock::now();
    }

    std::string getCurrentPlayer() const {
        // FIX: Menggunakan metode sideToMove(), bukan side_to_move
        return board_.sideToMove() == chess::Color::WHITE ? "white" : "black";
    }

    json getBoardState() {
        json state;
        //std::vector<std::vector<std::string>> board_2d(8, std::vector<std::string>(8, ""));
        //for (int r = 0; r < 8; ++r) {
        //    for (int c = 0; c < 8; ++c) {
        //        // FIX: Konversi (row, col) ke Square menggunakan constructor Rank dan File
        //        chess::Square sq{ chess::File(c), chess::Rank(r) };
        //        auto piece = board_.at(sq);
        //        if (piece != chess::Piece::NONE) {
        //            // FIX: Konversi Piece ke string menggunakan static_cast<std::string>
        //            board_2d[7 - r][c] = static_cast<std::string>(piece);
        //        }
        //    }
        //}
        //chess::Square ep_square = board_.enpassantSq();
        //if (ep_square != chess::Square::NO_SQ) {
        //    // Jika ada target en passant, konversi ke string (misal: "e3")
        //    state["enPassant"] = static_cast<std::string>(ep_square);
        //}
        //else {
        //    // Jika tidak ada, kirim "-"
        //    state["enPassant"] = "-";
        //}
        state["fen"] = board_.getFen(); // Langsung ambil FEN dari library

 /*       state["board"] = board_2d;*/
        state["currentPlayer"] = getCurrentPlayer();
        state["gameActive"] = gameActive;
        state["gameResult"] = gameResult;
        state["whiteTime"] = whiteTimeRemainingMs_;
        state["blackTime"] = blackTimeRemainingMs_;
        state["isAiGame"] = isAiGame_;
        state["whitePlayerName"] = whitePlayerName_;
        state["whitePlayerRating"] = whitePlayerRating_;
        state["blackPlayerName"] = blackPlayerName_;
        state["blackPlayerRating"] = blackPlayerRating_;
        return state;
    }

    bool makeMove(int fromRow, int fromCol, int toRow, int toCol, const std::string& promotionPiece = "") {
        if (!gameActive) return false;

        // Konversi (row, col) ke Square menggunakan constructor Rank dan File
        chess::Square from{ chess::Rank(7 - fromRow), chess::File(fromCol) };
        chess::Square to{ chess::Rank(7 - toRow), chess::File(toCol) };

        // Konversi string promosi dari klien ("q", "n", dll.) ke tipe dari library
        chess::PieceType promotionType = chess::PieceType::NONE;
        if (!promotionPiece.empty()) {
            promotionType = chess::PieceType(promotionPiece);
        }

        chess::Movelist legal_moves;
        chess::movegen::legalmoves(legal_moves, board_);

        for (const auto& legal_move : legal_moves) {
            // FIX: Bandingkan menggunakan .index() untuk menghindari error C2678
            if (legal_move.from() == from && legal_move.to() == to) {

                // Untuk client yang otomatis promosi Ratu, cari langkah promosi yang sesuai
                if (legal_move.typeOf() == chess::Move::PROMOTION) {
                    if (legal_move.promotionType() != chess::PieceType::QUEEN) {
                        continue; // Abaikan promosi selain Ratu jika client hanya mengirim itu
                    }
                }

                board_.makeMove(legal_move);

                if (getCurrentPlayer() == "white") {
                    whiteTimeRemainingMs_ += incrementMs_;
                }
                else {
                    blackTimeRemainingMs_ += incrementMs_;
                }

                // Pengecekan game over dari sisi server
                auto gameStatus = board_.isGameOver();
                if (gameStatus.first != chess::GameResultReason::NONE) {
                    if (gameStatus.second == chess::GameResult::LOSE) { // Checkmate
                        endGame(getCurrentPlayer() == "white" ? "black" : "white", "checkmate");
                    }
                    else { // Stalemate, repetition, dll.
                        endGame("", "draw");
                    }
                }
                return true;
            }
            // Cek apakah ini langkah castling yang cocok dengan input dari klien
            if (legal_move.typeOf() == chess::Move::CASTLING && legal_move.from().index() == from.index()) {
                // Tentukan apakah ini kingside atau queenside castling dari posisi benteng
                bool isKingSide = legal_move.to().file() > legal_move.from().file();

                // Dapatkan kotak tujuan raja yang seharusnya untuk castling ini
                chess::Square kingDestinationSquare = chess::Square::castling_king_square(isKingSide, board_.sideToMove());

                // Bandingkan dengan kotak tujuan yang dikirim oleh klien
                if (kingDestinationSquare.index() == to.index()) {
                    board_.makeMove(legal_move);
                    // ... (kode pengecekan game over di sini juga untuk konsistensi)
                    auto gameStatus = board_.isGameOver();
                    if (gameStatus.first != chess::GameResultReason::NONE) {
                        if (gameStatus.second == chess::GameResult::LOSE) {
                            endGame(getCurrentPlayer() == "white" ? "black" : "white", "checkmate");
                        }
                        else {
                            endGame("", "draw");
                        }
                    }
                    return true;
                }
            }
        }


        return false; // Langkah tidak ditemukan di daftar legal
    }
};


// Forward declaration
class ChessSession;

class ChessServer {
private:
    std::map<std::string, std::shared_ptr<ChessGame>> games_;
    std::vector<std::shared_ptr<ChessSession>> sessions_;
    mutable std::mutex sessions_mutex_;
    unsigned int next_game_id_;

public:
    ChessServer() : next_game_id_(0) {}

    void addSession(std::shared_ptr<ChessSession> session);
    void createGame(std::shared_ptr<ChessSession> session, long long initialTimeMs, long long incrementMs);
    void joinGame(std::shared_ptr<ChessSession> session, const std::string& game_id);
    void broadcastLobbyUpdate();
    void removeSession(std::shared_ptr<ChessSession> session);
    void broadcastMessage(const std::string& message, const std::string& game_id = "", std::shared_ptr<ChessSession> exclude = nullptr);
    std::shared_ptr<ChessGame> getGame(const std::string& game_id);
    size_t getSessionCount() const;
    void clearGameAssociationAndBroadcastLobby(const std::string& game_id);
    void rejoinGame(std::shared_ptr<ChessSession> session, const std::string& game_id, const std::string& player_color);
    void createAiGame(std::shared_ptr<ChessSession> session, long long initialTimeMs, long long incrementMs);
    void removeGame(const std::string& game_id);
    void processOnlineGameEnd(const std::string& game_id, const std::string& winner);
};

class ChessSession : public std::enable_shared_from_this<ChessSession> {
private:
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    ChessServer& server_;
    std::string playerColor_;
    std::string game_id_;
    std::atomic<bool> is_closing_{ false };
    std::mutex send_mutex_;
    std::queue<std::string> send_queue_;
    std::string username_ = "Guest";
    int rating_ = 0;

public:
    ChessSession(tcp::socket&& socket, ChessServer& server)
        : ws_(std::move(socket)), server_(server) {}

    void setUsername(const std::string& user) { username_ = user; }
    const std::string& getUsername() const { return username_; }
    void setRating(int rating) { rating_ = rating; }
    int getRating() const { return rating_; }
    void handleAuth(const json& data) {
        if (data.contains("username") && data["username"].is_string()) {
            setUsername(data["username"].get<std::string>());
        }
        if (data.contains("rating") && data["rating"].is_number_integer()) {
            setRating(data["rating"].get<int>());
        }

        std::cout << "[AUTH] Session authenticated for user: "
            << getUsername() << " (" << getRating() << ")" << std::endl;
    }

    void run(http::request<http::string_body> req) {
        try {
            net::dispatch(ws_.get_executor(),
                beast::bind_front_handler(
                    &ChessSession::on_run_with_req,
                    shared_from_this(),
                    std::move(req)));
        }
        catch (const std::exception& e) {
            std::cerr << "Exception in run(req): " << e.what() << std::endl;
            is_closing_.store(true);
        }
    }

    void run() {
        try {
            net::dispatch(ws_.get_executor(),
                beast::bind_front_handler(&ChessSession::on_run, shared_from_this()));
        }
        catch (const std::exception& e) {
            std::cerr << "Exception in run: " << e.what() << std::endl;
            is_closing_.store(true);
        }
    }

    void on_run_with_req(http::request<http::string_body> req) {
        // Accept handshake dengan req (upgrade)
        ws_.async_accept(req,
            beast::bind_front_handler(
                &ChessSession::on_accept,
                shared_from_this()));
    }

    void send(const std::string& message) {
            if (is_closing_.load()) {
                std::cout << "[ChessSession::send] Session is closing, skipping send." << std::endl;
                return;
            }

            if (!ws_.is_open()) {
                std::cout << "[ChessSession::send] WebSocket not open, calling fail to clean up session." << std::endl;
                // Call fail to ensure the session is properly removed from the server.
                // Use a default constructed error_code as this is a state check, not an async operation error.
                fail(beast::error_code{}, "send - socket not open");
                return;
            }

            std::lock_guard<std::mutex> guard(send_mutex_);
            
            // Check again after acquiring lock
            if (is_closing_.load()) {
                std::cout << "[ChessSession::send] Session is closing (after lock), skipping send." << std::endl;
                return;
            }

            bool was_empty = send_queue_.empty();
            send_queue_.push(message);
            std::cout << "[ChessSession::send] Message added to queue. Queue size: " << send_queue_.size() << std::endl;

            if (was_empty) {
                std::cout << "[ChessSession::send] Queue was empty, dispatching do_write." << std::endl;
                net::dispatch(ws_.get_executor(),
                    beast::bind_front_handler(&ChessSession::do_write, shared_from_this()));
            } else {
                std::cout << "[ChessSession::send] Write operation already in progress or queue not empty." << std::endl;
            }
    }

    

    const std::string& getPlayerColor() const { return playerColor_; }
    void setPlayerColor(const std::string& color) { playerColor_ = color; }

    const std::string& getGameId() const { return game_id_; }
    void setGameId(const std::string& id) { game_id_ = id; }

    auto get_executor() { return ws_.get_executor(); }

    bool isClosing() const { return is_closing_.load(); }

private:
    void do_close() {
        // Kirim pesan penutupan koneksi ke client
        ws_.async_close(websocket::close_code::normal,
            beast::bind_front_handler(
                &ChessSession::on_close,
                shared_from_this()));
    }
    void on_close(beast::error_code ec) {
        if (ec) {
            // Ini normal terjadi jika client menutup koneksi secara paksa
            // jadi kita tidak perlu mencetaknya sebagai error besar.
            // std::cerr << "close: " << ec.message() << "\n";
        }

        // Sekarang koneksi sudah benar-benar ditutup,
        // inilah saat yang aman untuk menghapus sesi dari server.
        server_.removeSession(shared_from_this());
    }
    void on_run() {
        try {
            // Set suggested timeout settings for the websocket
            ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

            // Set a decorator to change the Server of the handshake
            ws_.set_option(websocket::stream_base::decorator([](websocket::response_type& res) {
                res.set(http::field::server, std::string(BOOST_BEAST_VERSION_STRING) + " chess-server");
                }));

            // Accept the websocket handshake
            ws_.async_accept(beast::bind_front_handler(&ChessSession::on_accept, shared_from_this()));
        }
        catch (const std::exception& e) {
            std::cerr << "Exception in on_run: " << e.what() << std::endl;
            is_closing_.store(true);
        }
    }

    void do_write() {
            // No lock here, as send() already acquired it and dispatched on the same strand.
            // is_writing_ is already true from send()

            std::string const& message = send_queue_.front();

            ws_.async_write(net::buffer(message),
                beast::bind_front_handler(&ChessSession::on_write, shared_from_this()));
    }

    void on_accept(beast::error_code ec) {
        try {
            if (ec) return fail(ec, "accept");
            std::cout << "WebSocket accepted successfully" << std::endl;

            // Add this session to the server
            server_.addSession(shared_from_this());

            // Read a message
            do_read();
        }
        catch (const std::exception& e) {
            std::cerr << "Exception in on_accept: " << e.what() << std::endl;
            is_closing_.store(true);
        }
    }

    void do_read() {
        try {
            if (is_closing_.load()) return;

            // Check if websocket is still open
            if (!ws_.is_open()) {
                is_closing_.store(true);
                return;
            }

            // Read a message into our buffer
            ws_.async_read(buffer_,
                beast::bind_front_handler(&ChessSession::on_read, shared_from_this()));
        }
        catch (const std::exception& e) {
            std::cerr << "Exception in do_read: " << e.what() << std::endl;
            is_closing_.store(true);
        }
    }

    void on_read(beast::error_code ec, std::size_t bytes_transferred) {
        boost::ignore_unused(bytes_transferred);

        if (ec == websocket::error::closed) {
            std::cout << "WebSocket closed normally" << std::endl;
            is_closing_.store(true);
            return fail(ec, "read - normal close");
        }

        if (ec) {
            std::cout << "WebSocket read error: " << ec.message() << std::endl;
            is_closing_.store(true);
            return fail(ec, "read");
        }

        // Process the message
        std::string message = beast::buffers_to_string(buffer_.data());
        handleMessage(message);

        // Clear the buffer
        buffer_.consume(buffer_.size());

        // Read another message
        do_read();
    }

    void on_write(beast::error_code ec, std::size_t bytes_transferred) {
            boost::ignore_unused(bytes_transferred);

            // If there was an error, call fail to handle it and remove the session.
            if (ec) {
                return fail(ec, "write"); // Call fail for any error, including closed connection
            }

            // If no error, remove the sent message from the queue.
            std::lock_guard<std::mutex> guard(send_mutex_);
            send_queue_.pop();
            std::cout << "[ChessSession::on_write] Message popped from queue. Remaining size: " << send_queue_.size() << std::endl;

            // If there are more messages and the session is not closing, dispatch the next write.
            if (!send_queue_.empty() && !is_closing_.load()) {
                std::cout << "[ChessSession::on_write] Queue not empty, dispatching do_write." << std::endl;
                net::dispatch(ws_.get_executor(),
                    beast::bind_front_handler(&ChessSession::do_write, shared_from_this()));
            }
    }

    

    void handleMessage(const std::string& message) {
        if (is_closing_.load()) return;

            json data = json::parse(message);

            if (!data.contains("type") || !data["type"].is_string()) {
                json error;
                error["type"] = "error";
                error["message"] = "Invalid message format: missing or invalid type field";
                send(error.dump());
                return;
            }

            std::string type = data["type"];

            if (type == "createGame") {
                if (data.contains("timeControl") && data["timeControl"].is_object()) {
                    long long initialMs = data["timeControl"].value("initialMs", 300000LL); // Default 5 menit
                    long long incrementMs = data["timeControl"].value("incrementMs", 0LL);   // Default 0 detik
                    server_.createGame(shared_from_this(), initialMs, incrementMs);
                }
                else {
                    // Fallback jika data tidak dikirim (misal dari klien versi lama)
                    server_.createGame(shared_from_this(), 300000, 0);
                }
            }
            else if (type == "joinGame") {
                if (!data.contains("gameId") || !data["gameId"].is_string()) {
                    json error;
                    error["type"] = "error";
                    error["message"] = "Invalid joinGame message: missing or invalid gameId field";
                    send(error.dump());
                    return;
                }
                std::string gameId = data["gameId"];
                server_.joinGame(shared_from_this(), gameId);
            }
            else if (type == "auth") {
                handleAuth(data);
            }
            // Di dalam ChessSession::handleMessage, tambahkan setelah blok "joinGame"

            else if (type == "createAiGame") {
                // FIX: Baca objek timeControl dari JSON
                if (data.contains("timeControl") && data["timeControl"].is_object()) {
                    long long initialMs = data["timeControl"].value("initialMs", 300000LL);
                    long long incrementMs = data["timeControl"].value("incrementMs", 0LL);
                    server_.createAiGame(shared_from_this(), initialMs, incrementMs);
                }
                else {
                    // Fallback
                    server_.createAiGame(shared_from_this(), 300000, 0);
                }
            }
            else if (type == "move") {
                std::shared_ptr<ChessGame> game = server_.getGame(game_id_);
                if (!game) {
                    json error;
                    error["type"] = "error";
                    error["message"] = "Game not found for this session";
                    send(error.dump());
                    return;
                }

                // Untuk game manusia vs manusia, periksa apakah giliran pemain sudah benar.
                if (!game->isAiGame() && game->getCurrentPlayer() != playerColor_) {
                    json error;
                    error["type"] = "error";
                    error["message"] = "Not your turn";
                    send(error.dump());
                    return;
                }

                // Validate incoming move data format
                if (!data.contains("fromRow") || !data.contains("fromCol") ||
                    !data.contains("toRow") || !data.contains("toCol") ||
                    !data["fromRow"].is_number_integer() || !data["fromCol"].is_number_integer() ||
                    !data["toRow"].is_number_integer() || !data["toCol"].is_number_integer()) {
                    json error;
                    error["type"] = "error";
                    error["message"] = "Invalid move data";
                    send(error.dump());
                    return;
                }

                // First, handle time. The player whose turn it was loses time.
                std::string movingPlayer = game->getCurrentPlayer();
                game->updateTime();

                // Check for timeout BEFORE processing the move.
                if (game->checkTimeout()) {
                    std::string winner = (movingPlayer == "white") ? "black" : "white"; // The other player wins
                    game->endGame(winner, "timeout");

                    json gameEnd;
                    gameEnd["type"] = "gameEnded";
                    gameEnd["reason"] = "timeout";
                    gameEnd["winner"] = winner;
                    server_.broadcastMessage(gameEnd.dump(), game_id_);
                    server_.clearGameAssociationAndBroadcastLobby(game_id_);
                    return; // Game ended by timeout, no need to process the move.
                }

                // Now, attempt to make the move.
                int fromRow = data["fromRow"];
                int fromCol = data["fromCol"];
                int toRow = data["toRow"];
                int toCol = data["toCol"];
                std::string promotion = data.value("promotion", "");

                if (game->makeMove(fromRow, fromCol, toRow, toCol)) {
                    if (!game->isActive()) {
                        // Jika game sudah tidak aktif (karena checkmate atau remis)
                        std::string winner = (game->getCurrentPlayer() == "white") ? "black" : "white";
                        server_.processOnlineGameEnd(game_id_, winner);
                        std::cout << "[GAME END] Move resulted in game over." << std::endl;

                        // Buat pesan 'gameEnded' yang JELAS, bukan 'boardUpdate'
                        json gameEnd;
                        gameEnd["type"] = "gameEnded";
                        // Ambil hasil game yang sudah diatur oleh endGame() di dalam makeMove
                        auto result = game->getBoardState();
                        gameEnd["reason"] = result["gameResult"]; // Misal: "white wins by checkmate"
                        gameEnd["winner"] = (result["gameResult"].get<std::string>().find("white") != std::string::npos) ? "white" : "black";
                        if (result["gameResult"].get<std::string>().find("draw") != std::string::npos) {
                            gameEnd["winner"] = ""; // Kosongkan jika remis
                        }

                        // Siarkan hasil akhir
                        server_.broadcastMessage(gameEnd.dump(), game_id_);

                        // Lakukan pembersihan total
                        server_.removeGame(game_id_);
                        server_.clearGameAssociationAndBroadcastLobby(game_id_);
                    }
                    else {
                        // Jika game MASIH AKTIF, kirim boardUpdate seperti biasa
                        json moveResponse;
                        moveResponse["type"] = "boardUpdate";
                        moveResponse["gameState"] = game->getBoardState();
                        moveResponse["lastMove"] = {
                            {"fromRow", fromRow}, {"fromCol", fromCol},
                            {"toRow", toRow}, {"toCol", toCol}
                        };
                        server_.broadcastMessage(moveResponse.dump(), game_id_);
                    }
                }
                else {
                    json error;
                    error["type"] = "error";
                    error["message"] = "Invalid move";
                    send(error.dump());
                }
            }
            else if (type == "resign") {
                std::shared_ptr<ChessGame> game = server_.getGame(game_id_);
                if (!game) { /* ... error handling ... */ return; }
                std::string winner = (playerColor_ == "white") ? "black" : "white";
                game->endGame(winner, "resignation");

                // Panggil fungsi perhitungan rating sebelum membersihkan game
                server_.processOnlineGameEnd(game_id_, winner);

                json gameEnd;
                gameEnd["type"] = "gameEnded";
                gameEnd["reason"] = "resignation";
                gameEnd["winner"] = winner;
                gameEnd["resignedPlayer"] = playerColor_;

                server_.broadcastMessage(gameEnd.dump(), game_id_);
                server_.removeGame(game_id_);
                server_.clearGameAssociationAndBroadcastLobby(game_id_);
            }
            else if (type == "gameOver") {
                std::shared_ptr<ChessGame> game = server_.getGame(data["gameId"].get<std::string>());
                if (!game) {
                    json error;
                    error["type"] = "error";
                    error["message"] = "Game not found for gameOver message";
                    send(error.dump());
                    return;
                }

                std::string reason = data["reason"].get<std::string>();
                std::string winner = data["winner"].get<std::string>();

                game->endGame(winner, reason);
                // Panggil fungsi perhitungan rating sebelum membersihkan game
                server_.processOnlineGameEnd(game_id_, winner);

                json gameEnd;
                gameEnd["type"] = "gameEnded";
                gameEnd["reason"] = reason;
                gameEnd["winner"] = winner;

                server_.broadcastMessage(gameEnd.dump(), data["gameId"].get<std::string>());
                server_.clearGameAssociationAndBroadcastLobby(data["gameId"].get<std::string>());
            }
            else if (type == "newGame") {
                std::shared_ptr<ChessGame> game = server_.getGame(game_id_);
                if (!game) { /* ... error handling ... */ return; }
                game->resetBoard();

                json newGameResponse;
                newGameResponse["type"] = "newGame";
                newGameResponse["gameState"] = game->getBoardState();
                server_.broadcastMessage(newGameResponse.dump(), game_id_);
            }
            else if (type == "rejoinGame") {
                if (!data.contains("gameId") || !data["gameId"].is_string() ||
                    !data.contains("playerColor") || !data["playerColor"].is_string()) {
                    json error;
                    error["type"] = "error";
                    error["message"] = "Invalid rejoinGame message: missing gameId or playerColor";
                    send(error.dump());
                    return;
                }
                std::string gameId = data["gameId"];
                std::string color = data["playerColor"];
                server_.rejoinGame(shared_from_this(), gameId, color);
            }
            else if (type == "claimTimeout") {
                std::shared_ptr<ChessGame> game = server_.getGame(game_id_);
                if (!game || !game->isActive()) { return; }
                game->updateTime(); // Verifikasi waktu berdasarkan jam server

            if (game->checkTimeout()) {
                // Klaim valid, akhiri permainan
                std::string winner = playerColor_;
                game->endGame(winner, "timeout");

                json gameEnd;
                gameEnd["type"] = "gameEnded";
                gameEnd["reason"] = "timeout";
                gameEnd["winner"] = winner;
                // Panggil fungsi perhitungan rating sebelum membersihkan game
                server_.processOnlineGameEnd(game_id_, winner);
                server_.broadcastMessage(gameEnd.dump(), game_id_);

                server_.removeGame(game_id_);
                server_.clearGameAssociationAndBroadcastLobby(game_id_);
            }
            else {
                // Klaim tidak valid, kirim error
                json error;
                error["type"] = "error";
                error["message"] = "Klaim ditolak, lawan masih memiliki waktu.";
                send(error.dump());
            }
}
            else {
                json error;
                error["type"] = "error";
                error["message"] = "Unknown message type: " + type;
                send(error.dump());
            }
    }

    void fail(beast::error_code ec, char const* what) {
        // Jika cleanup sudah berjalan, jangan lakukan apa-apa.
        if (is_closing_.exchange(true)) {
            return;
        }

        // Jangan log penutupan koneksi normal sebagai error
        if (ec && ec != websocket::error::closed && ec != net::error::eof) {
            std::cerr << what << ": " << ec.message() << "\n";
        }

        // Alih-alih memanggil removeSession secara langsung,
        // post tugas untuk menutup koneksi secara teratur pada strand.
        // Ini memastikan operasi close tidak akan berbenturan dengan operasi write.
        net::post(ws_.get_executor(),
            beast::bind_front_handler(
                &ChessSession::do_close,
                shared_from_this()));
    }
};

// ChessServer implementation
void ChessServer::addSession(std::shared_ptr<ChessSession> session) {
    try {
        std::cout << "[addSession] New session received." << std::endl;
        
        { // New scope for the lock
            std::lock_guard<std::mutex> guard(sessions_mutex_);
            std::cout << "[addSession] Mutex locked." << std::endl;

            // Initialize session with no game assigned
            session->setPlayerColor(""); // No color assigned yet
            session->setGameId("");      // Not in a game yet
            sessions_.push_back(session);

            std::cout << "[addSession] Session added to lobby." << std::endl;
            std::cout << "[addSession] Mutex unlocked." << std::endl;
        } // Mutex is unlocked here

        // Broadcast lobby update to all clients
        broadcastLobbyUpdate();
    }
    catch (const std::exception& e) {
        std::cerr << "Exception in addSession: " << e.what() << std::endl;
    }
}

void ChessServer::removeSession(std::shared_ptr<ChessSession> session) {
    try {
        std::cout << "[ChessServer::removeSession] Attempting to remove session." << std::endl;
        std::string game_id_to_notify;
        std::string disconnected_color;
        json gameEnd_message;

        // Variabel untuk menahan session agar tidak langsung dihancurkan
        std::shared_ptr<ChessSession> session_to_destroy = nullptr;

        { // Kurung kurawal untuk membatasi scope lock_guard
            std::lock_guard<std::mutex> guard(sessions_mutex_);

            auto it = sessions_.begin();
            while (it != sessions_.end()) {
                if (it->get() == session.get()) {
                    std::cout << "[ChessServer::removeSession] Found session to remove." << std::endl;

                    // 1. Simpan session yang akan dihapus ke variabel lokal
                    session_to_destroy = *it;

                    game_id_to_notify = (*it)->getGameId();
                    disconnected_color = (*it)->getPlayerColor();
                    it = sessions_.erase(it); // 2. Hapus dari vector, ref count turun 1
                    break;
                }
                else {
                    ++it;
                }
            }

            // ... (sisa logika untuk mengecek game dan membuat pesan gameEnd_message) ...
            // Pastikan semua logika yang mengakses 'games_' atau 'sessions_' tetap di dalam scope ini.
            if (!game_id_to_notify.empty()) {
                auto game_it = games_.find(game_id_to_notify);
                if (game_it != games_.end()) {
                    auto game = game_it->second;
                    int active_players_in_game = 0;
                    for (const auto& s : sessions_) {
                        if (!s->isClosing() && s->getGameId() == game_id_to_notify) {
                            active_players_in_game++;
                        }
                    }
                    if (active_players_in_game == 0) {
                        if (game->isActive()) {
                            std::string winner = (disconnected_color == "white") ? "black" : "white";
                            game->endGame(winner, "disconnect");
                            gameEnd_message["type"] = "gameEnded";
                            gameEnd_message["reason"] = "disconnect";
                            gameEnd_message["winner"] = winner;
                            gameEnd_message["disconnectedPlayer"] = disconnected_color;
                        }
                        games_.erase(game_it);
                        std::cout << "Game " << game_id_to_notify << " has ended and been removed due to no active players." << std::endl;
                    }
                    else {
                        json opponentDisconnectedMsg;
                        opponentDisconnectedMsg["type"] = "playerDisconnected";
                        opponentDisconnectedMsg["message"] = "Your opponent has disconnected. Waiting for them to rejoin.";
                        broadcastMessage(opponentDisconnectedMsg.dump(), game_id_to_notify, session);
                        std::cout << "Game " << game_id_to_notify << " remains active with " << active_players_in_game << " player(s)." << std::endl;
                    }
                }
            }

        } // 3. Mutex dilepaskan di sini saat 'guard' keluar dari scope

        // Broadcast pesan di luar lock untuk mencegah deadlock
        if (!gameEnd_message.is_null()) {
            broadcastMessage(gameEnd_message.dump(), game_id_to_notify);
        }
        broadcastLobbyUpdate();

    }
    catch (const std::exception& e) {
        std::cerr << "Exception in removeSession: " << e.what() << std::endl;
    }
    // 4. Jika 'session_to_destroy' adalah pemilik terakhir,
    //    destructor ~ChessSession() akan dipanggil di sini, SETELAH mutex dilepaskan.
}

void ChessServer::createGame(std::shared_ptr<ChessSession> session, long long initialTimeMs, long long incrementMs) {
    std::string game_id;
    { // New scope for the lock
        std::lock_guard<std::mutex> guard(sessions_mutex_);

        game_id = "game_" + std::to_string(next_game_id_++);
        auto game = std::make_shared<ChessGame>(initialTimeMs, incrementMs); // Pass initialTimeMs
        games_[game_id] = game;

        session->setGameId(game_id);
        session->setPlayerColor("white");

        std::cout << "Player created and joined game " << game_id << " as white." << std::endl;
    } // Mutex is unlocked here

    json playerAssigned_msg;
    playerAssigned_msg["type"] = "playerAssigned";
    playerAssigned_msg["color"] = "white";
    playerAssigned_msg["gameId"] = game_id;
    session->send(playerAssigned_msg.dump());

    broadcastLobbyUpdate();
}

void ChessServer::joinGame(std::shared_ptr<ChessSession> session, const std::string& game_id) {
    json playerAssigned_msg;
    json boardUpdate_msg;
    json gameStart_msg;
    bool game_started = false;
    std::shared_ptr<ChessGame> game; // Declare game here
    std::shared_ptr<ChessSession> whitePlayerSession = nullptr;
    std::shared_ptr<ChessSession> blackPlayerSession = nullptr;

    { // New scope for the lock
        std::lock_guard<std::mutex> guard(sessions_mutex_);

        auto game_it = games_.find(game_id);
        if (game_it == games_.end()) {
            json error;
            error["type"] = "error";
            error["message"] = "Game not found: " + game_id;
            net::post(session->get_executor(), [session, msg = error.dump()]() { session->send(msg); });
            return;
        }
        game = game_it->second; // Assign to the declared game variable

        int players_in_game = 0;
        for (const auto& s : sessions_) {
            if (s->getGameId() == game_id) {
                players_in_game++;
            }
        }

        if (players_in_game >= 2) {
            json error;
            error["type"] = "error";
            error["message"] = "Game is full: " + game_id;
            net::post(session->get_executor(), [session, msg = error.dump()]() { session->send(msg); });
            return;
        }

        session->setGameId(game_id);
        session->setPlayerColor("black");
        blackPlayerSession = session;
        for (const auto& s : sessions_) {
            if (s->getGameId() == game_id && s != blackPlayerSession) {
                whitePlayerSession = s;
                break;
            }
        }
        if (whitePlayerSession && blackPlayerSession) {
            std::string namaPutih = whitePlayerSession->getUsername();
            std::string namaHitam = blackPlayerSession->getUsername();

            // Panggil fungsi untuk menyimpan nama di objek game
            //game->setPlayerNames(namaPutih, namaHitam);

            // (Opsional) Lakukan hal yang sama untuk rating
             int ratingPutih = whitePlayerSession->getRating();
             int ratingHitam = blackPlayerSession->getRating();
             game->setPlayerNamesAndRatings(namaPutih, ratingPutih, namaHitam, ratingHitam);
        }


        std::cout << "Player joined game " << game_id << " as black." << std::endl;

        // Prepare messages
        playerAssigned_msg["type"] = "playerAssigned";
        playerAssigned_msg["color"] = "black";
        playerAssigned_msg["gameId"] = game_id;

        boardUpdate_msg["type"] = "boardUpdate";
        boardUpdate_msg["gameState"] = game->getBoardState();

        game_started = true;
        gameStart_msg["type"] = "gameStart";
        gameStart_msg["message"] = "Game starting with 2 players!";
        gameStart_msg["gameId"] = game_id;
    } // Mutex is unlocked here

    if (game_started) {
        // Get both sessions in the game
        std::vector<std::shared_ptr<ChessSession>> game_sessions;
        {
            std::lock_guard<std::mutex> guard(sessions_mutex_);
            for (const auto& s : sessions_) {
                if (s->getGameId() == game_id) {
                    game_sessions.push_back(s);
                }
            }
        }

        for (const auto& s : game_sessions) {
            json current_playerAssigned_msg;
            current_playerAssigned_msg["type"] = "playerAssigned";
            current_playerAssigned_msg["color"] = s->getPlayerColor();
            current_playerAssigned_msg["gameId"] = s->getGameId();

            json current_boardUpdate_msg;
            current_boardUpdate_msg["type"] = "boardUpdate";
            current_boardUpdate_msg["gameState"] = game->getBoardState();

            net::post(s->get_executor(), [s, msg = current_playerAssigned_msg.dump()]() { s->send(msg); });
            net::post(s->get_executor(), [s, msg = current_boardUpdate_msg.dump()]() { s->send(msg); });
            net::post(s->get_executor(), [s, msg = gameStart_msg.dump()]() { s->send(msg); });
        }
        broadcastLobbyUpdate(); // Update lobby for all
    }
}

void ChessServer::broadcastLobbyUpdate() {
    json lobbyUpdate_msg;
    lobbyUpdate_msg["type"] = "lobbyUpdate";

    // Gunakan json::array untuk menampung objek game
    json waiting_games = json::array();

    {
        std::lock_guard<std::mutex> guard(sessions_mutex_);
        for (const auto& pair : games_) {
            std::string game_id = pair.first;
            auto game = pair.second;

            int players_in_game = 0;
            for (const auto& s : sessions_) {
                if (s->getGameId() == game_id) {
                    players_in_game++;
                }
            }

            // Hanya broadcast game yang sedang menunggu lawan
            if (players_in_game == 1) {
                // Ambil data waktu dari objek game
                long long initialMs = game->getInitialTimeMs();
                long long incrementMs = game->getIncrementMs();

                // Konversi kembali ke format string "menit+detik"
                long long minutes = initialMs / 60000;
                long long seconds = incrementMs / 1000;
                std::string time_format = std::to_string(minutes) + "+" + std::to_string(seconds);

                // Buat objek JSON dan masukkan ke array
                json game_info;
                game_info["id"] = game_id;
                game_info["time"] = time_format;
                waiting_games.push_back(game_info);
            }
        }
    }

    lobbyUpdate_msg["games"] = waiting_games;

    // Broadcast to lobby (sessions with no game_id)
    std::vector<std::shared_ptr<ChessSession>> lobby_sessions;
    {
        std::lock_guard<std::mutex> guard(sessions_mutex_);
        for (const auto& session : sessions_) {
            if (session && !session->isClosing() && session->getGameId().empty()) {
                lobby_sessions.push_back(session);
            }
        }
    }

    for (const auto& session : lobby_sessions) {
        net::post(session->get_executor(), [session, msg = lobbyUpdate_msg.dump()]() {
            session->send(msg);
        });
    }
}

void ChessServer::broadcastMessage(const std::string& message, const std::string& game_id, std::shared_ptr<ChessSession> exclude) {
    std::vector<std::shared_ptr<ChessSession>> sessions_to_send;
    {
        std::lock_guard<std::mutex> guard(sessions_mutex_);
        for (const auto& session : sessions_) {
            if (session && !session->isClosing() && (game_id.empty() || session->getGameId() == game_id)) {
                if (session != exclude) {
                    sessions_to_send.push_back(session);
                }
            }
        }
    }

    std::cout << "Broadcasting message to " << sessions_to_send.size() << " sessions (game_id: " << (game_id.empty() ? "all" : game_id) << "): " << message << std::endl;

    for (const auto& session : sessions_to_send) {
        // Buat weak_ptr dari shared_ptr
        std::weak_ptr<ChessSession> weak_session = session;

        net::post(session->get_executor(), [weak_session, message]() {
            // Coba "kunci" weak_ptr untuk mendapatkan shared_ptr yang aman
            if (auto session = weak_session.lock()) {
                // JIKA BERHASIL: Objek 'session' dijamin masih hidup
                // selama scope blok 'if' ini. Aman untuk memanggil send().
                session->send(message);
            }
            // JIKA GAGAL: Objek 'session' sudah dihancurkan.
            // Tidak ada tindakan yang perlu diambil, pesan tidak terkirim.
            });
    }
}

void ChessServer::processOnlineGameEnd(const std::string& game_id, const std::string& winner) {
    std::shared_ptr<ChessSession> whitePlayer = nullptr;
    std::shared_ptr<ChessSession> blackPlayer = nullptr;

    // Kunci mutex untuk mengakses data sesi
    std::lock_guard<std::mutex> guard(sessions_mutex_);

    // 1. Temukan kedua sesi pemain dalam game yang berakhir
    for (const auto& s : sessions_) {
        if (s->getGameId() == game_id) {
            if (s->getPlayerColor() == "white") whitePlayer = s;
            else if (s->getPlayerColor() == "black") blackPlayer = s;
        }
    }

    if (whitePlayer && blackPlayer) {
        // 2. Ambil rating mereka saat ini
        int whiteRating = whitePlayer->getRating();
        int blackRating = blackPlayer->getRating();

        // 3. Tentukan hasil dan hitung rating baru
        if (winner == "white") {
            calculateNewRatings(whiteRating, blackRating, 1.0); // Putih menang
            calculateNewRatings(blackRating, whiteRating, 0.0); // Hitam kalah
        }
        else if (winner == "black") {
            calculateNewRatings(whiteRating, blackRating, 0.0); // Putih kalah
            calculateNewRatings(blackRating, whiteRating, 1.0); // Hitam menang
        }
        else { // Remis
            calculateNewRatings(whiteRating, blackRating, 0.5);
            calculateNewRatings(blackRating, whiteRating, 0.5);
        }

        // 4. Perbarui rating di sesi mereka
        whitePlayer->setRating(whiteRating);
        blackPlayer->setRating(blackRating);

        std::cout << "[RATING] New ratings calculated. White: " << whiteRating << ", Black: " << blackRating << std::endl;
        
        // (Di masa depan, di sini Anda akan mengirim rating baru ke database PHP)

        // 5. Kirim update rating ke setiap klien
        json whiteUpdate;
        whiteUpdate["type"] = "ratingUpdate";
        whiteUpdate["newRating"] = whiteRating;
        whitePlayer->send(whiteUpdate.dump());

        json blackUpdate;
        blackUpdate["type"] = "ratingUpdate";
        blackUpdate["newRating"] = blackRating;
        blackPlayer->send(blackUpdate.dump());
    }
}

size_t ChessServer::getSessionCount() const {
    std::lock_guard<std::mutex> guard(sessions_mutex_);
    return sessions_.size();
}

std::shared_ptr<ChessGame> ChessServer::getGame(const std::string& game_id) {
    std::lock_guard<std::mutex> guard(sessions_mutex_); // Protect access to games_ map
    auto it = games_.find(game_id);
    if (it != games_.end()) {
        return it->second;
    }
    return nullptr; // Game not found
}

void ChessServer::clearGameAssociationAndBroadcastLobby(const std::string& game_id) {
    // Kurung kurawal ini penting untuk membatasi masa hidup lock_guard
    {
        std::lock_guard<std::mutex> guard(sessions_mutex_);
        for (const auto& s : sessions_) {
            if (s && s->getGameId() == game_id) {
                // Modifikasi state session di dalam lock
                s->setGameId("");
                s->setPlayerColor("");
            }
        }
    } // <-- Mutex dilepaskan di sini saat 'guard' keluar dari scope

    // Panggil broadcastLobbyUpdate SETELAH mutex dilepaskan.
    // Sekarang tidak akan ada deadlock.
    broadcastLobbyUpdate();
}

void ChessServer::rejoinGame(std::shared_ptr<ChessSession> session, const std::string& game_id, const std::string& player_color) {
    std::lock_guard<std::mutex> guard(sessions_mutex_);

    std::cout << "[ChessServer::rejoinGame] Rejoin attempt for game: " << game_id << ", color: " << player_color << std::endl; // New log

    auto game_it = games_.find(game_id);
    if (game_it == games_.end()) {
        json error;
        error["type"] = "error";
        error["message"] = "Rejoin failed: Game not found.";
        session->send(error.dump());
        std::cout << "[ChessServer::rejoinGame] Error: Game not found." << std::endl; // New log
        return;
    }

    std::shared_ptr<ChessGame> game = game_it->second;

    // Check if the game is already full or the player color is taken
    int players_in_game = 0;
    bool color_taken = false;
    for (const auto& s : sessions_) {
        if (!s->isClosing() && s->getGameId() == game_id) {
            players_in_game++;
            if (s->getPlayerColor() == player_color) {
                color_taken = true; // This color is already associated with an active session
            }
        }
    }

    std::cout << "[ChessServer::rejoinGame] Current players in game " << game_id << ": " << players_in_game << std::endl; // New log
    std::cout << "[ChessServer::rejoinGame] Is color " << player_color << " taken? " << (color_taken ? "Yes" : "No") << std::endl; // New log

    if (players_in_game >= 2) {
        json error;
        error["type"] = "error";
        error["message"] = "Rejoin failed: Game is full.";
        session->send(error.dump());
        std::cout << "[ChessServer::rejoinGame] Error: Game is full." << std::endl; // New log
        return;
    }

    if (color_taken) {
        json error;
        error["type"] = "error";
        error["message"] = "Rejoin failed: Your color is already taken in this game.";
        session->send(error.dump());
        std::cout << "[ChessServer::rejoinGame] Error: Color already taken." << std::endl; // New log
        return;
    }

    // Re-associate the session
    session->setGameId(game_id);
    session->setPlayerColor(player_color);

    std::cout << "Player rejoined game " << game_id << " as " << player_color << "." << std::endl;

    // Send current game state to the rejoining player
    json rejoinResponse;
    rejoinResponse["type"] = "rejoinSuccess";
    rejoinResponse["gameId"] = game_id;
    rejoinResponse["color"] = player_color;
    rejoinResponse["gameState"] = game->getBoardState();
    session->send(rejoinResponse.dump());
    std::cout << "[ChessServer::rejoinGame] Sent rejoinSuccess to " << player_color << " for game " << game_id << std::endl; // New log
}
void ChessServer::createAiGame(std::shared_ptr<ChessSession> session, long long initialTimeMs, long long incrementMs) {
    std::string game_id;
    std::shared_ptr<ChessGame> game;

    { // Scope untuk mutex
        std::lock_guard<std::mutex> guard(sessions_mutex_);
        game_id = "game_" + std::to_string(next_game_id_++);

        game = std::make_shared<ChessGame>(initialTimeMs, incrementMs);
        game->setAsAiGame(true); // PASTIKAN BARIS INI ADA
        games_[game_id] = game;

        // Pemain manusia selalu Putih
        session->setGameId(game_id);
        session->setPlayerColor("white");

        std::cout << "Player created and started AI game " << game_id << " as white." << std::endl;
    } // Mutex dilepaskan

    // Kirim konfirmasi ke pemain
    json playerAssigned_msg;
    playerAssigned_msg["type"] = "playerAssigned";
    playerAssigned_msg["color"] = "white";
    playerAssigned_msg["gameId"] = game_id;
    session->send(playerAssigned_msg.dump());

    // Langsung mulai game karena tidak perlu menunggu pemain kedua
    json gameStart_msg;
    gameStart_msg["type"] = "gameStart";
    gameStart_msg["message"] = "Game starting against AI!";
    gameStart_msg["gameId"] = game_id;
    session->send(gameStart_msg.dump());
    
    // Kirim status papan awal
    json boardUpdate_msg;
    boardUpdate_msg["type"] = "boardUpdate";
    boardUpdate_msg["gameState"] = game->getBoardState();
    session->send(boardUpdate_msg.dump());

    // Update lobi untuk pemain lain (menghapus game ini dari daftar tunggu)
    broadcastLobbyUpdate();
}

void ChessServer::removeGame(const std::string& game_id) {
    std::lock_guard<std::mutex> guard(sessions_mutex_);
    auto it = games_.find(game_id);
    if (it != games_.end()) {
        games_.erase(it);
        std::cout << "[Game Cleanup] Game " << game_id << " has been removed." << std::endl;
    }
}

// === HttpSession: handle HTTP atau upgrade ke WebSocket ===
class HttpSession : public std::enable_shared_from_this<HttpSession> {
    tcp::socket socket_;
    beast::flat_buffer buffer_;
    ChessServer& server_;
public:
    HttpSession(tcp::socket socket, ChessServer& server)
        : socket_(std::move(socket)), server_(server) {}

    void run() {
        auto self = shared_from_this();
        auto req = std::make_shared<http::request<http::string_body>>();

        http::async_read(socket_, buffer_, *req,
            [self, req](beast::error_code ec, std::size_t bytes_transferred) {
                boost::ignore_unused(bytes_transferred);
                if (ec) {
                    std::cerr << "HTTP read error: " << ec.message() << std::endl;
                    return;
                }

                if (websocket::is_upgrade(*req)) {
                    auto session = std::make_shared<ChessSession>(
                        std::move(self->socket_), self->server_);
                    session->run(std::move(*req));  //  sekarang pass request
                }
                else {
                    // Kalau HTTP biasa -> balas file
                    self->handle_http(std::move(*req));
                }
            });
    }

    //void handle_http(http::request<http::string_body> req) {
    //    std::string target = std::string(req.target());
    //    if (target == "/") target = "/index.html";
    //    std::string path = "www" + target;

    //    auto res = std::make_shared<http::response<http::string_body>>();  //  pakai shared_ptr

    //    std::ifstream file(path, std::ios::binary);
    //    if (file) {
    //        std::stringstream ss;
    //        ss << file.rdbuf();
    //        res->result(http::status::ok);
    //        res->version(req.version());

    //        if (path.find(".html") != std::string::npos)
    //            res->set(http::field::content_type, "text/html");
    //        else if (path.find(".js") != std::string::npos)
    //            res->set(http::field::content_type, "application/javascript");
    //        else if (path.find(".css") != std::string::npos)
    //            res->set(http::field::content_type, "text/css");
    //        else
    //            res->set(http::field::content_type, "text/plain");

    //        res->body() = ss.str();
    //    }
    //    else {
    //        *res = { http::status::not_found, req.version() };
    //        res->set(http::field::content_type, "text/plain");
    //        res->body() = "404 - Not Found";
    //    }

    //    res->prepare_payload();

    //    auto self = shared_from_this();
    //    http::async_write(socket_, *res,
    //        [self, res](beast::error_code ec, std::size_t) {
    //            if (ec) {
    //                std::cerr << "HTTP write error: " << ec.message() << std::endl;
    //            }
    //              Optional: tutup socket setelah balas HTTP
    //            beast::error_code ignore_ec;
    //            self->socket_.shutdown(tcp::socket::shutdown_send, ignore_ec);
    //        });
    //}
    void handle_http(http::request<http::string_body> req) {
        std::string target = std::string(req.target());
        if (target == "/") target = "/index.html";
        std::string path = "www" + target;

        auto res = std::make_shared<http::response<http::string_body>>();

        std::ifstream file(path, std::ios::binary);
        if (file) {
            std::stringstream ss;
            ss << file.rdbuf();
            res->result(http::status::ok);
            res->version(req.version());

            // Tentukan Content-Type
            if (path.find(".html") != std::string::npos)
                res->set(http::field::content_type, "text/html");
            else if (path.find(".js") != std::string::npos)
                res->set(http::field::content_type, "application/javascript");
            else if (path.find(".css") != std::string::npos)
                res->set(http::field::content_type, "text/css");
            else if (path.find(".wasm") != std::string::npos || path.find(".asm") != std::string::npos)
                res->set(http::field::content_type, "application/wasm");
            else
                res->set(http::field::content_type, "text/plain");

            // Tambahkan header cache khusus untuk file statis
            if (path.find(".js") != std::string::npos ||
                path.find(".wasm") != std::string::npos ||
                path.find(".asm") != std::string::npos)
            {
                res->set(http::field::cache_control, "public, max-age=31536000"); // cache 1 tahun
                res->set(http::field::pragma, "cache");

                // Buat ETag sederhana dari ukuran file + timestamp
                try {
                    namespace fs = std::filesystem;
                    auto fsize = fs::file_size(path);
                    auto ftime = fs::last_write_time(path).time_since_epoch().count();
                    std::ostringstream etag;
                    etag << "\"" << fsize << "-" << ftime << "\"";
                    res->set(http::field::etag, etag.str());
                }
                catch (...) {
                    // kalau gagal baca metadata, abaikan
                }
            }

            res->body() = ss.str();
        }
        else {
            *res = { http::status::not_found, req.version() };
            res->set(http::field::content_type, "text/plain");
            res->body() = "404 - Not Found";
        }

        res->prepare_payload();

        auto self = shared_from_this();
        http::async_write(socket_, *res,
            [self, res](beast::error_code ec, std::size_t) {
                if (ec) {
                    std::cerr << "HTTP write error: " << ec.message() << std::endl;
                }
                // Tutup socket setelah kirim respons
                beast::error_code ignore_ec;
                self->socket_.shutdown(tcp::socket::shutdown_send, ignore_ec);
            });
    }


};

// Accepts incoming connections and launches the sessions
class Listener : public std::enable_shared_from_this<Listener> {
private:
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    ChessServer& server_;

public:
    Listener(net::io_context& ioc, tcp::endpoint endpoint, ChessServer& server)
        : ioc_(ioc), acceptor_(net::make_strand(ioc)), server_(server) {
        beast::error_code ec;

        // Open the acceptor
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            fail(ec, "open");
            return;
        }

        // Allow address reuse
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            fail(ec, "set_option");
            return;
        }

        // Bind to the server address
        acceptor_.bind(endpoint, ec);
        if (ec) {
            fail(ec, "bind");
            return;
        }

        // Start listening for connections
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            fail(ec, "listen");
            return;
        }
    }

    void run() {
        do_accept();
    }

private:
    void do_accept() {
        // The new connection gets its own strand
        acceptor_.async_accept(net::make_strand(ioc_),
            beast::bind_front_handler(&Listener::on_accept, shared_from_this()));
    }

    void on_accept(beast::error_code ec, tcp::socket socket) {
        if (ec) {
            fail(ec, "accept");
        }
        else {
            // Create the session and run it
            std::make_shared<HttpSession>(std::move(socket), server_)->run();
        }

        // Accept another connection
        do_accept();
    }

    void fail(beast::error_code ec, char const* what) {
        std::cerr << what << ": " << ec.message() << "\n";
    }
};

int main(int argc, char* argv[]) {
    try {
        auto const address = net::ip::make_address("0.0.0.0");
        auto const port = static_cast<unsigned short>(80);
        auto const threads = std::max<int>(1, 4);

        // The io_context is required for all I/O
        net::io_context ioc{ threads };

        // Create the chess server
        ChessServer server;

        // Create and launch a listening port
        auto listener = std::make_shared<Listener>(ioc, tcp::endpoint{ address, port }, server);
        listener->run();

        std::cout << "Chess WebSocket Server started on port " << port << std::endl;
        std::cout << "Connect with WebSocket client to ws://localhost:" << port << std::endl;

        // Run the I/O service on the requested number of threads
        std::vector<std::thread> v;
        v.reserve(threads - 1);
        for (auto i = threads - 1; i > 0; --i) {
            v.emplace_back([&ioc] {
                try {
                    ioc.run();
                }
                catch (const std::exception& e) {
                    std::cerr << "Exception in worker thread: " << e.what() << std::endl;
                }
                });
        }

        try {
            ioc.run();
        }
        catch (const std::exception& e) {
            std::cerr << "Exception in main thread: " << e.what() << std::endl;
        }

        // Wait for all threads to complete
        for (auto& t : v) {
            if (t.joinable()) {
                t.join();
            }
        }

    }
    catch (const std::exception& e) {
        std::cerr << "Fatal exception in main: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}