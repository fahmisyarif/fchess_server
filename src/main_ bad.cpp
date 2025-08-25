#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/json.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <queue>

// Ganti nlohmann/json dengan Boost.JSON untuk konsistensi
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace json = boost::json;
using tcp = net::ip::tcp;

// Forward declarations
class ChessSession;
class ChessServer;

// ============================================================================
// ChessGame Class
// ============================================================================
class ChessGame {
private:
    std::vector<std::vector<std::string>> board;
    std::string currentPlayer;
    bool gameActive;
    std::string gameResult;

public:
    ChessGame() {
        resetBoard();
    }

    void resetBoard() {
        board = {
            {"r", "n", "b", "q", "k", "b", "n", "r"},
            {"p", "p", "p", "p", "p", "p", "p", "p"},
            {"", "", "", "", "", "", "", ""},
            {"", "", "", "", "", "", "", ""},
            {"", "", "", "", "", "", "", ""},
            {"", "", "", "", "", "", "", ""},
            {"P", "P", "P", "P", "P", "P", "P", "P"},
            {"R", "N", "B", "Q", "K", "B", "N", "R"}
        };
        currentPlayer = "white";
        gameActive = true;
        gameResult = "";
    }

    json::object getBoardState() const {
        json::object state;
        json::array json_board;
        for (const auto& row : board) {
            json_board.push_back(json::array(row.begin(), row.end()));
        }
        state["board"] = json_board;
        state["currentPlayer"] = currentPlayer;
        state["gameActive"] = gameActive;
        state["gameResult"] = gameResult;
        return state;
    }

    bool makeMove(int fromRow, int fromCol, int toRow, int toCol) {
        if (!gameActive) return false;
        if (fromRow < 0 || fromRow >= 8 || fromCol < 0 || fromCol >= 8) return false;
        if (toRow < 0 || toRow >= 8 || toCol < 0 || toCol >= 8) return false;

        std::string piece = board[fromRow][fromCol];
        if (piece.empty()) return false;

        bool isWhitePiece = (piece[0] >= 'A' && piece[0] <= 'Z');
        if ((currentPlayer == "white" && !isWhitePiece) ||
            (currentPlayer == "black" && isWhitePiece)) {
            return false;
        }

        board[toRow][toCol] = piece;
        board[fromRow][fromCol] = "";

        currentPlayer = (currentPlayer == "white") ? "black" : "white";
        return true;
    }

    void endGame(const std::string& winner, const std::string& reason) {
        gameActive = false;
        gameResult = winner + " wins by " + reason;
    }

    bool isActive() const { return gameActive; }
    std::string getCurrentPlayer() const { return currentPlayer; }
};

// ============================================================================
// ChessServer Class
// ============================================================================
class ChessServer {
private:
    std::map<std::string, std::shared_ptr<ChessGame>> games_;
    std::vector<std::weak_ptr<ChessSession>> sessions_; // Use weak_ptr
    std::map<std::string, std::shared_ptr<ChessSession>> sessions_by_game_;
    mutable std::mutex mutex_;
    unsigned int next_game_id_;

public:
    ChessServer() : next_game_id_(0) {}

    void addSession(std::shared_ptr<ChessSession> session);
    void removeSession(const std::shared_ptr<ChessSession>& session);
    void createGame(const std::shared_ptr<ChessSession>& session);
    void joinGame(const std::shared_ptr<ChessSession>& session, const std::string& game_id);
    void broadcastLobbyUpdate();
    void broadcastMessage(const std::string& message, const std::string& game_id = "", const std::shared_ptr<ChessSession>& exclude = nullptr);
    std::shared_ptr<ChessGame> getGame(const std::string& game_id);
    void clearGameAssociationAndBroadcastLobby(const std::string& game_id);
};

// ============================================================================
// ChessSession Class
// ============================================================================
class ChessSession : public std::enable_shared_from_this<ChessSession> {
private:
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    ChessServer& server_;
    std::string playerColor_;
    std::string game_id_;
    std::atomic<bool> is_closing_ = false;

    // Tambahkan timer untuk timeout
    net::steady_timer close_timer_;

public:
    ChessSession(tcp::socket&& socket, ChessServer& server)
        : ws_(std::move(socket)), server_(server), close_timer_(ws_.get_executor()) {}

    ~ChessSession() {
        if (!is_closing_.exchange(true)) {
            // Hindari pemanggilan rekursif ke server_
        }
    }

    void run();
    void send(const std::string& message);

    const std::string& getPlayerColor() const { return playerColor_; }
    void setPlayerColor(const std::string& color) { playerColor_ = color; }

    const std::string& getGameId() const { return game_id_; }
    void setGameId(const std::string& id) { game_id_ = id; }

    net::any_io_executor get_executor() { return ws_.get_executor(); }
    bool isClosing() const { return is_closing_.load(); }

private:
    void on_run();
    void do_read();
    void on_accept(beast::error_code ec);
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void handleMessage(const std::string& message);
    void fail(beast::error_code ec, const char* what);
};

// ============================================================================
// ChessServer Implementations (Simplified for clarity)
// ============================================================================
void ChessServer::addSession(std::shared_ptr<ChessSession> session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.push_back(session);
    broadcastLobbyUpdate();
}

void ChessServer::removeSession(const std::shared_ptr<ChessSession>& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (auto s = it->lock()) {
            if (s == session) {
                // Hapus asosiasi game
                std::string game_id = s->getGameId();
                if (!game_id.empty()) {
                    auto game_it = games_.find(game_id);
                    if (game_it != games_.end()) {
                        games_.erase(game_it);
                    }
                }
                it = sessions_.erase(it);
                broadcastLobbyUpdate();
                return;
            }
            else {
                ++it;
            }
        }
        else {
            // Hapus weak_ptr yang sudah kadaluarsa
            it = sessions_.erase(it);
        }
    }
}

void ChessServer::createGame(const std::shared_ptr<ChessSession>& session) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string game_id = "game_" + std::to_string(next_game_id_++);
    auto game = std::make_shared<ChessGame>();
    games_[game_id] = game;
    session->setGameId(game_id);
    session->setPlayerColor("white");
    broadcastLobbyUpdate();
}

void ChessServer::joinGame(const std::shared_ptr<ChessSession>& session, const std::string& game_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto game_it = games_.find(game_id);
    if (game_it == games_.end()) {
        // Kirim pesan error
        json::object error_msg;
        error_msg["type"] = "error";
        error_msg["message"] = "Game not found";
        session->send(json::serialize(error_msg));
        return;
    }
    session->setGameId(game_id);
    session->setPlayerColor("black");
    broadcastLobbyUpdate();
    // Kirim pesan game start, update board, dll
}

void ChessServer::broadcastLobbyUpdate() {
    std::lock_guard<std::mutex> lock(mutex_);
    json::array waiting_games;
    for (const auto& pair : games_) {
        // Logika untuk menentukan game yang menunggu
        // ...
        waiting_games.emplace_back(pair.first);
    }
    json::object lobby_update;
    lobby_update["type"] = "lobbyUpdate";
    lobby_update["games"] = waiting_games;
    broadcastMessage(json::serialize(lobby_update));
}

void ChessServer::broadcastMessage(const std::string& message, const std::string& game_id, const std::shared_ptr<ChessSession>& exclude) {
    std::vector<std::shared_ptr<ChessSession>> recipients;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto const& weak_session : sessions_) {
            if (auto session = weak_session.lock()) {
                if (session != exclude && (game_id.empty() || session->getGameId() == game_id)) {
                    recipients.push_back(session);
                }
            }
        }
    }
    for (const auto& session : recipients) {
        net::post(session->get_executor(), [session, message]() {
            session->send(message);
            });
    }
}

std::shared_ptr<ChessGame> ChessServer::getGame(const std::string& game_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = games_.find(game_id);
    if (it != games_.end()) {
        return it->second;
    }
    return nullptr;
}

void ChessServer::clearGameAssociationAndBroadcastLobby(const std::string& game_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto const& weak_session : sessions_) {
        if (auto s = weak_session.lock()) {
            if (s->getGameId() == game_id) {
                s->setGameId("");
                s->setPlayerColor("");
            }
        }
    }
    auto game_it = games_.find(game_id);
    if (game_it != games_.end()) {
        games_.erase(game_it);
    }
    broadcastLobbyUpdate();
}

// ============================================================================
// ChessSession Implementations
// ============================================================================
void ChessSession::run() {
    net::dispatch(ws_.get_executor(),
        beast::bind_front_handler(&ChessSession::on_run, shared_from_this()));
}

void ChessSession::on_run() {
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws_.set_option(websocket::stream_base::decorator([](websocket::response_type& res) {
        res.set(http::field::server, std::string(BOOST_BEAST_VERSION_STRING) + " chess-server");
        }));
    ws_.async_accept(beast::bind_front_handler(&ChessSession::on_accept, shared_from_this()));
}

void ChessSession::on_accept(beast::error_code ec) {
    if (ec) return fail(ec, "accept");
    server_.addSession(shared_from_this());
    do_read();
}

void ChessSession::do_read() {
    if (is_closing_.load()) return;
    ws_.async_read(buffer_, beast::bind_front_handler(&ChessSession::on_read, shared_from_this()));
}

void ChessSession::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec == websocket::error::closed) {
        is_closing_.store(true);
        server_.removeSession(shared_from_this());
        return;
    }
    if (ec) {
        is_closing_.store(true);
        server_.removeSession(shared_from_this());
        return fail(ec, "read");
    }

    std::string message = beast::buffers_to_string(buffer_.data());
    handleMessage(message);

    buffer_.consume(buffer_.size());
    do_read();
}

void ChessSession::on_write(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        is_closing_.store(true);
        server_.removeSession(shared_from_this());
        return fail(ec, "write");
    }
}

void ChessSession::send(const std::string& message) {
    if (is_closing_.load() || !ws_.is_open()) return;

    // Use a queue to handle backpressure and serialize writes
    // (Implementasi asli sudah memiliki pola ini, tetapi sekarang dikaitkan dengan eksekutor)
    net::post(ws_.get_executor(), [self = shared_from_this(), message]() {
        bool is_writing = !self->ws_.is_open();
        if (!is_writing) {
            self->ws_.async_write(net::buffer(message),
                beast::bind_front_handler(&ChessSession::on_write, self));
        }
    });
}

void ChessSession::handleMessage(const std::string& message) {
    if (is_closing_.load()) return;

    try {
        json::value data = json::parse(message);
        const std::string type = json::value_to<std::string>(data.at("type"));

        if (type == "createGame") {
            server_.createGame(shared_from_this());
        }
        else if (type == "joinGame") {
            const std::string game_id = json::value_to<std::string>(data.at("gameId"));
            server_.joinGame(shared_from_this(), game_id);
        }
        else if (type == "move") {
            // Periksa game_id_ sebelum mendapatkan shared_ptr ke game
            if (game_id_.empty()) {
                // Kirim pesan error jika tidak ada game_id
                return;
            }

            auto game = server_.getGame(game_id_);
            if (!game) {
                // Game tidak ditemukan, sesi ini harusnya tidak bisa bermain
                return;
            }

            // ... (logika move yang sama seperti sebelumnya)
        }
        else if (type == "resign") {
            auto game = server_.getGame(game_id_);
            if (game) {
                std::string winner = (playerColor_ == "white") ? "black" : "white";
                game->endGame(winner, "resignation");
                server_.broadcastMessage("Game ended by resignation", game_id_);
                server_.clearGameAssociationAndBroadcastLobby(game_id_);
            }
        }
        // ... (handle message lain)
    }
    catch (const std::exception& e) {
        // Penanganan error JSON atau data yang tidak valid
        json::object error_msg;
        error_msg["type"] = "error";
        error_msg["message"] = "Error parsing message: " + std::string(e.what());
        send(json::serialize(error_msg));
    }
}

void ChessSession::fail(beast::error_code ec, const char* what) {
    if (ec == websocket::error::closed || ec == net::error::eof) {
        std::cout << "Connection closed normally." << std::endl;
    }
    else {
        std::cerr << what << ": " << ec.message() << std::endl;
    }
    is_closing_.store(true);
}

// ============================================================================
// Listener Class
// ============================================================================
class Listener : public std::enable_shared_from_this<Listener> {
private:
    net::io_context& ioc_;
    tcp::acceptor acceptor_;
    ChessServer& server_;
public:
    Listener(net::io_context& ioc, tcp::endpoint endpoint, ChessServer& server)
        : ioc_(ioc), acceptor_(net::make_strand(ioc)), server_(server) {
        beast::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        acceptor_.bind(endpoint, ec);
        acceptor_.listen(net::socket_base::max_listen_connections, ec);
    }
    void run() { do_accept(); }
private:
    void do_accept() {
        acceptor_.async_accept(net::make_strand(ioc_),
            beast::bind_front_handler(&Listener::on_accept, shared_from_this()));
    }
    void on_accept(beast::error_code ec, tcp::socket socket) {
        if (ec) {
            std::cerr << "Accept failed: " << ec.message() << std::endl;
        }
        else {
            std::make_shared<ChessSession>(std::move(socket), server_)->run();
        }
        do_accept();
    }
};

// ============================================================================
// Main Function
// ============================================================================
int main(int argc, char* argv[]) {
    try {
        auto const address = net::ip::make_address("0.0.0.0");
        auto const port = static_cast<unsigned short>(8080);
        auto const threads = std::max<int>(1, 4);

        net::io_context ioc{ threads };
        ChessServer server;
        auto listener = std::make_shared<Listener>(ioc, tcp::endpoint{ address, port }, server);
        listener->run();

        std::cout << "Chess WebSocket Server started on port " << port << std::endl;
        std::cout << "Connect with WebSocket client to ws://localhost:" << port << std::endl;

        std::vector<std::thread> v;
        v.reserve(threads - 1);
        for (auto i = threads - 1; i > 0; --i) {
            v.emplace_back([&ioc] {
                ioc.run();
                });
        }
        ioc.run();
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