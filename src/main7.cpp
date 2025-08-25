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
    std::map<std::string, std::shared_ptr<ChessGame>> games_;
    std::vector<std::shared_ptr<ChessSession>> sessions_;
    mutable std::mutex sessions_mutex_;
    unsigned int next_game_id_;

public:
    ChessServer() : next_game_id_(0) {}

    void addSession(std::shared_ptr<ChessSession> session);
    void createGame(std::shared_ptr<ChessSession> session);
    void joinGame(std::shared_ptr<ChessSession> session, const std::string& game_id);
    void broadcastLobbyUpdate();
    void removeSession(std::shared_ptr<ChessSession> session);
    void broadcastMessage(const std::string& message, const std::string& game_id = "", std::shared_ptr<ChessSession> exclude = nullptr);
    std::shared_ptr<ChessGame> getGame(const std::string& game_id);
    size_t getSessionCount() const;
    std::string assignPlayerColor(std::shared_ptr<ChessSession> session);
    void clearGameAssociationAndBroadcastLobby(const std::string& game_id);
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
            if (is_closing_.load()) {
                std::cout << "Session is closing, skipping send" << std::endl;
                return;
            }

            if (!ws_.is_open()) {
                std::cout << "WebSocket not open, skipping send" << std::endl;
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
            if (!is_writing_.load()) { // Check without exchange
                is_writing_.store(true); // Set to true here, as we are about to dispatch a write
                net::dispatch(ws_.get_executor(),
                    beast::bind_front_handler(&ChessSession::do_write, shared_from_this()));
            }
            else {
                std::cout << "Write operation already in progress" << std::endl;
            }
    }

    const std::string& getPlayerColor() const { return playerColor_; }
    void setPlayerColor(const std::string& color) { playerColor_ = color; }

    const std::string& getGameId() const { return game_id_; }
    void setGameId(const std::string& id) { game_id_ = id; }

    auto get_executor() { return ws_.get_executor(); }

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
                // Jika ada, panggil `do_write` untuk mengirim pesan berikutnya.
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
                server_.createGame(shared_from_this());
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
            else if (type == "move") {
                std::shared_ptr<ChessGame> game = server_.getGame(game_id_);
                if (!game) {
                    json error;
                    error["type"] = "error";
                    error["message"] = "Game not found for this session";
                    send(error.dump());
                    return;
                }

                if (game->getCurrentPlayer() != playerColor_) {
                    json error;
                    error["type"] = "error";
                    error["message"] = "Not your turn";
                    send(error.dump());
                    return;
                }

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
                    json moveResponse;
                    moveResponse["type"] = "boardUpdate";
                    moveResponse["gameState"] = game->getBoardState();
                    moveResponse["lastMove"] = {
                        {"fromRow", fromRow},
                        {"fromCol", fromCol},
                        {"toRow", toRow},
                        {"toCol", toCol}
                    };
                    server_.broadcastMessage(moveResponse.dump(), game_id_);
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

                json gameEnd;
                gameEnd["type"] = "gameEnded";
                gameEnd["reason"] = "resignation";
                gameEnd["winner"] = winner;
                gameEnd["resignedPlayer"] = playerColor_;
                server_.broadcastMessage(gameEnd.dump(), game_id_);

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
            else {
                json error;
                error["type"] = "error";
                error["message"] = "Unknown message type: " + type;
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
        std::string game_id_to_notify;
        std::string disconnected_color;
        json gameEnd_message;

        {
            std::lock_guard<std::mutex> guard(sessions_mutex_);

            auto it = sessions_.begin();
            while (it != sessions_.end()) {
                if (*it == session) {
                    game_id_to_notify = (*it)->getGameId();
                    disconnected_color = (*it)->getPlayerColor();
                    std::cout << "Player " << disconnected_color << " from game " << game_id_to_notify << " disconnected." << std::endl;
                    it = sessions_.erase(it);
                    break;
                } else {
                    ++it;
                }
            }

            if (!game_id_to_notify.empty()) {
                auto game_it = games_.find(game_id_to_notify);
                if (game_it != games_.end()) {
                    auto game = game_it->second;
                    if (game->isActive()) {
                        std::string winner = (disconnected_color == "white") ? "black" : "white";
                        game->endGame(winner, "disconnect");

                        gameEnd_message["type"] = "gameEnded";
                        gameEnd_message["reason"] = "disconnect";
                        gameEnd_message["winner"] = winner;
                        gameEnd_message["disconnectedPlayer"] = disconnected_color;
                    }
                    // Clean up the game if it's over
                    games_.erase(game_it);
                    std::cout << "Game " << game_id_to_notify << " has ended and been removed." << std::endl;
                }
            }
        } // Mutex is unlocked here

        // Broadcast the message outside the lock to prevent deadlock
        if (!gameEnd_message.is_null()) {
            broadcastMessage(gameEnd_message.dump(), game_id_to_notify);
        }
        broadcastLobbyUpdate();
    }
    catch (const std::exception& e) {
        std::cerr << "Exception in removeSession: " << e.what() << std::endl;
    }
}

void ChessServer::createGame(std::shared_ptr<ChessSession> session) {
    std::string game_id;
    { // New scope for the lock
        std::lock_guard<std::mutex> guard(sessions_mutex_);

        game_id = "game_" + std::to_string(next_game_id_++);
        auto game = std::make_shared<ChessGame>();
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
    std::vector<std::string> waiting_games;
    {
        std::lock_guard<std::mutex> guard(sessions_mutex_);
        for (const auto& pair : games_) {
            std::string game_id = pair.first;
            int players_in_game = 0;
            for (const auto& s : sessions_) {
                if (s->getGameId() == game_id) {
                    players_in_game++;
                }
            }
            if (players_in_game == 1) {
                waiting_games.push_back(game_id);
            }
        }
    }

    json lobbyUpdate_msg;
    lobbyUpdate_msg["type"] = "lobbyUpdate";
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
        // Post the send operation to the session's strand
        net::post(session->get_executor(), [session, message]() {
            session->send(message);
        });
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
    std::lock_guard<std::mutex> guard(sessions_mutex_);
    for (const auto& s : sessions_) {
        if (s && s->getGameId() == game_id) {
            s->setGameId("");
            s->setPlayerColor("");
        }
    }
    broadcastLobbyUpdate();
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