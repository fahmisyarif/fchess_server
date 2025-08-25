#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <queue>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

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

    json getBoardState() {
        json state;
        state["board"] = board;
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

        // Simple validation: white pieces are uppercase, black are lowercase
        bool isWhitePiece = (piece[0] >= 'A' && piece[0] <= 'Z');
        if ((currentPlayer == "white" && !isWhitePiece) ||
            (currentPlayer == "black" && isWhitePiece)) {
            return false;
        }

        // Make the move
        board[toRow][toCol] = piece;
        board[fromRow][fromCol] = "";

        // Switch players
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

// Forward declaration
class ChessSession;

class ChessServer {
private:
    std::shared_ptr<ChessGame> game_;
    std::vector<std::shared_ptr<ChessSession>> sessions_;
    mutable std::mutex sessions_mutex_;

public:
    ChessServer() : game_(std::make_shared<ChessGame>()) {}

    void addSession(std::shared_ptr<ChessSession> session);
    void removeSession(std::shared_ptr<ChessSession> session);
    void broadcastMessage(const std::string& message, std::shared_ptr<ChessSession> exclude = nullptr);
    std::shared_ptr<ChessGame> getGame() { return game_; }
    size_t getSessionCount() const;
    std::string assignPlayerColor(std::shared_ptr<ChessSession> session);
};

class ChessSession : public std::enable_shared_from_this<ChessSession> {
private:
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer buffer_;
    ChessServer& server_;
    std::string playerColor_;
    std::atomic<bool> is_closing_{ false };
    std::mutex send_mutex_;
    std::queue<std::string> send_queue_;
    std::atomic<bool> is_writing_{ false };

public:
    ChessSession(tcp::socket&& socket, ChessServer& server)
        : ws_(std::move(socket)), server_(server) {}

    ~ChessSession() {
        // Prevent recursive calls during destruction
        if (!is_closing_.exchange(true)) {
            try {
                // Close WebSocket gracefully
                if (ws_.is_open()) {
                    ws_.close(websocket::close_code::going_away);
                }
            }
            catch (...) {
                // Ignore errors during cleanup
            }
            server_.removeSession(shared_from_this());
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

    void send(const std::string& message) {
        try {
            if (is_closing_.load()) {
                std::cout << "Session is closing, skipping send" << std::endl;
                return;
            }

            std::lock_guard<std::mutex> guard(send_mutex_);

            if (is_closing_.load()) {
                std::cout << "Session is closing (after lock), skipping send" << std::endl;
                return;
            }

            // Add message to queue
            send_queue_.push(message);

            // Start writing if not already writing
            if (!is_writing_.exchange(true)) {
                net::dispatch(ws_.get_executor(),
                    beast::bind_front_handler(&ChessSession::do_write, shared_from_this()));
            }
            else {
                std::cout << "Write operation already in progress" << std::endl;
            }
        }
        catch (const std::exception& e) {
            std::cerr << "Exception in send: " << e.what() << std::endl;
            is_closing_.store(true);
        }
    }

    const std::string& getPlayerColor() const { return playerColor_; }
    void setPlayerColor(const std::string& color) { playerColor_ = color; }

    bool isClosing() const { return is_closing_.load(); }

private:
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
            // Ambil pesan pertama dari antrean
            std::string const& message = send_queue_.front();

            // Mulai operasi tulis asinkron
            // boost::beast::async_write akan mengirim pesan dan memanggil on_write setelah selesai
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
        try {
            boost::ignore_unused(bytes_transferred);

            if (ec == websocket::error::closed) {
                std::cout << "WebSocket closed normally" << std::endl;
                is_closing_.store(true);
                return;
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
        catch (const std::exception& e) {
            std::cerr << "Exception in on_read: " << e.what() << std::endl;
            is_closing_.store(true);
        }
    }

    void on_write(beast::error_code ec, std::size_t bytes_transferred) {
            // Pastikan flag `is_writing_` direset, karena operasi tulis telah selesai.
            is_writing_.store(false);

            // Cek apakah ada error saat operasi tulis.
            if (ec) {
                // Log error dengan pesan yang lebih jelas.
                std::cerr << "WebSocket write error: " << ec.message() << std::endl;

                // Pengecualian jika koneksi ditutup secara normal
                if (ec != websocket::error::closed) {
                    is_closing_.store(true);
                }

                // Jika ada error, hentikan pemrosesan antrean.
                return;
            }

            // Jika tidak ada error, hapus pesan yang baru saja terkirim dari antrean.
            std::lock_guard<std::mutex> guard(send_mutex_);
            send_queue_.pop();

            // Periksa apakah antrean masih memiliki pesan dan sesi masih aktif.
            if (!send_queue_.empty() && !is_closing_.load()) {
                // Jika ada, setel flag `is_writing_` lagi dan panggil `do_write` untuk mengirim pesan berikutnya.
                is_writing_.store(true);
                net::dispatch(ws_.get_executor(),
                    beast::bind_front_handler(&ChessSession::do_write, shared_from_this()));
            }
    }

    void handleMessage(const std::string& message) {
        if (is_closing_.load()) return;

        try {
            json data = json::parse(message);

            // Check if required field exists
            if (!data.contains("type") || !data["type"].is_string()) {
                json error;
                error["type"] = "error";
                error["message"] = "Invalid message format: missing or invalid type field";
                send(error.dump());
                return;
            }

            std::string type = data["type"];
            auto game = server_.getGame();

            if (!game) {
                json error;
                error["type"] = "error";
                error["message"] = "Game not available";
                send(error.dump());
                return;
            }

            if (type == "move") {
                // Validate it's the player's turn
                if (game->getCurrentPlayer() != playerColor_) {
                    json error;
                    error["type"] = "error";
                    error["message"] = "Not your turn";
                    send(error.dump());
                    return;
                }

                // Validate move data exists and is numeric
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

                int fromRow = data["fromRow"];
                int fromCol = data["fromCol"];
                int toRow = data["toRow"];
                int toCol = data["toCol"];

                if (game->makeMove(fromRow, fromCol, toRow, toCol)) {
                    // Broadcast the move to all players
                    json moveResponse;
                    moveResponse["type"] = "boardUpdate";
                    moveResponse["gameState"] = game->getBoardState();
                    moveResponse["lastMove"] = {
                        {"fromRow", fromRow},
                        {"fromCol", fromCol},
                        {"toRow", toRow},
                        {"toCol", toCol}
                    };

                    server_.broadcastMessage(moveResponse.dump());
                }
                else {
                    json error;
                    error["type"] = "error";
                    error["message"] = "Invalid move";
                    send(error.dump());
                }
            }
            else if (type == "resign") {
                std::string winner = (playerColor_ == "white") ? "black" : "white";
                game->endGame(winner, "resignation");

                json gameEnd;
                gameEnd["type"] = "gameEnded";
                gameEnd["reason"] = "resignation";
                gameEnd["winner"] = winner;
                gameEnd["resignedPlayer"] = playerColor_;

                server_.broadcastMessage(gameEnd.dump());
            }
            else if (type == "newGame") {
                game->resetBoard();

                json newGameResponse;
                newGameResponse["type"] = "newGame";
                newGameResponse["gameState"] = game->getBoardState();

                server_.broadcastMessage(newGameResponse.dump());
            }
            else {
                json error;
                error["type"] = "error";
                error["message"] = "Unknown message type: " + type;
                send(error.dump());
            }

        }
        catch (const json::parse_error& e) {
            std::cerr << "JSON parse error: " << e.what() << std::endl;
            json error;
            error["type"] = "error";
            error["message"] = "Invalid JSON format";
            send(error.dump());
        }
        catch (const json::type_error& e) {
            std::cerr << "JSON type error: " << e.what() << std::endl;
            json error;
            error["type"] = "error";
            error["message"] = "Invalid JSON data types";
            send(error.dump());
        }
        catch (const std::exception& e) {
            std::cerr << "Error processing message: " << e.what() << std::endl;
            json error;
            error["type"] = "error";
            error["message"] = "Server error processing message";
            send(error.dump());
        }
    }

    void fail(beast::error_code ec, char const* what) {
        // Only log if not a normal close
        if (ec != websocket::error::closed && ec != net::error::eof) {
            std::cerr << what << ": " << ec.message() << "\n";
        }
        is_closing_.store(true);
    }
};

// ChessServer implementation
void ChessServer::addSession(std::shared_ptr<ChessSession> session) {
    try {
        std::string playerColor;
        bool shouldStartGame = false;

        {
            std::lock_guard<std::mutex> guard(sessions_mutex_);

            if (sessions_.size() == 0) {
                playerColor = "white";
            }
            else if (sessions_.size() == 1) {
                playerColor = "black";
                shouldStartGame = true; // Two players are now connected
            }
            else {
                std::cout << "Connection rejected: server full" << std::endl;
                return;
            }

            session->setPlayerColor(playerColor);
            sessions_.push_back(session);
        }

        std::cout << "Player connected as " << playerColor << ". Total players: " << getSessionCount() << std::endl;

        // Send player assignment
        json playerAssigned;
        playerAssigned["type"] = "playerAssigned";
        playerAssigned["color"] = playerColor;
        std::cout << "Sending playerAssigned: " << playerAssigned.dump() << std::endl;
        session->send(playerAssigned.dump());

        // Send current board state
        json boardUpdate;
        boardUpdate["type"] = "boardUpdate";
        boardUpdate["gameState"] = game_->getBoardState();
        std::cout << "Sending boardUpdate: " << boardUpdate.dump() << std::endl;
        session->send(boardUpdate.dump());

        // If both players are now connected, notify both of them
        if (shouldStartGame) {
            std::cout << "Both players connected, starting game!" << std::endl;

            json gameStart;
            gameStart["type"] = "gameStart";
            gameStart["message"] = "Game starting with 2 players!";
            std::cout << "Broadcasting gameStart: " << gameStart.dump() << std::endl;
            broadcastMessage(gameStart.dump());
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Exception in addSession: " << e.what() << std::endl;
    }
}

void ChessServer::removeSession(std::shared_ptr<ChessSession> session) {
    try {
        bool shouldNotifyDisconnect = false;
        std::string disconnectedColor;

        {
            std::lock_guard<std::mutex> guard(sessions_mutex_);

            // Use iterator to safely remove
            auto it = sessions_.begin();
            while (it != sessions_.end()) {
                if (*it == session) {
                    disconnectedColor = (*it)->getPlayerColor();
                    std::cout << "Player " << disconnectedColor << " disconnected" << std::endl;
                    it = sessions_.erase(it);
                    shouldNotifyDisconnect = !sessions_.empty();
                    break;
                }
                else {
                    ++it;
                }
            }
        }

        // Notify remaining players outside the lock
        if (shouldNotifyDisconnect) {
            json disconnect;
            disconnect["type"] = "playerDisconnected";
            disconnect["disconnectedPlayer"] = disconnectedColor;
            broadcastMessage(disconnect.dump());
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Exception in removeSession: " << e.what() << std::endl;
    }
}

void ChessServer::broadcastMessage(const std::string& message, std::shared_ptr<ChessSession> exclude) {
    try {
        std::vector<std::shared_ptr<ChessSession>> sessionsCopy;

        {
            std::lock_guard<std::mutex> guard(sessions_mutex_);

            // Create a copy to avoid iterator invalidation
            sessionsCopy.reserve(sessions_.size());
            for (const auto& session : sessions_) {
                if (session && !session->isClosing()) {
                    sessionsCopy.push_back(session);
                }
            }
        }

        std::cout << "Broadcasting message to " << sessionsCopy.size() << " sessions: " << message << std::endl;

        // Send messages outside the lock
        for (const auto& session : sessionsCopy) {
            if (session && session != exclude && !session->isClosing()) {
                session->send(message);
            }
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Exception in broadcastMessage: " << e.what() << std::endl;
    }
}

size_t ChessServer::getSessionCount() const {
    std::lock_guard<std::mutex> guard(sessions_mutex_);
    return sessions_.size();
}

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
            std::make_shared<ChessSession>(std::move(socket), server_)->run();
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
        auto const port = static_cast<unsigned short>(8080);
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