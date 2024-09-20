#include <iostream>
#include <vector>
#include <eigen3/Eigen/Dense>

#include <thread>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <queue>

#include "common.h"

struct Input
{
    uint32_t id;
    Player player;
};

struct Client
{
    sockaddr_in address;
    std::queue<Input> queue;
    bool regulateQueue;
    size_t playerId;
    std::chrono::steady_clock::time_point lastUpdate;
};

void receive(int &udpSocket, std::vector<Client> &clients)
{
    while (true)
    {
        char buffer[48];
        struct sockaddr_in clientAddress;
        socklen_t clientAddressLength = sizeof(clientAddress);
        int recvLength = recvfrom(udpSocket, buffer, sizeof(buffer), 0, (struct sockaddr *)&clientAddress, &clientAddressLength);
        if (recvLength == 0)
        {
            close(udpSocket);
            // TODO: catch exceptions
            throw std::runtime_error("error receiving input");
        }

        Client *client = nullptr;
        for (Client &c : clients)
        {
            if (c.address.sin_addr.s_addr == clientAddress.sin_addr.s_addr && c.address.sin_port == clientAddress.sin_port)
            {
                client = &c;
                break;
            }
        }

        if (client == nullptr)
        {
            if (clients.size() < 2)
            {
                const uint8_t playerId = clients.size() == 0 ? 0 : clients[0].playerId ^ 1;
                clients.push_back(Client{.address = clientAddress, .regulateQueue = true, .playerId = playerId, .lastUpdate = std::chrono::steady_clock::now()});
                // TODO: deal with packet loss
                sendto(udpSocket, &playerId, sizeof(playerId), 0, (sockaddr *)&clientAddress, sizeof(sockaddr));
            }
            else
            {
                continue;
            }
        }
        else
        {
            Input input;
            std::memcpy(&input.id, buffer, 4);
            std::memcpy(input.player.carState.position.data(), buffer + 4, 12);
            std::memcpy(input.player.carState.velocity.data(), buffer + 16, 12);
            std::memcpy(input.player.carState.orientation.data(), buffer + 28, 12);
            std::memcpy(&input.player.action.steering, buffer + 40, 4);
            std::memcpy(&input.player.action.throttle, buffer + 44, 4);
            client->queue.push(input);
            client->lastUpdate = std::chrono::steady_clock::now();
        }
    }
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <Port>" << std::endl;
        return EXIT_FAILURE;
    }

    int udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSocket < 0)
    {
        std::cerr << "error creating udp socket" << std::endl;
        return EXIT_FAILURE;
    }

    struct sockaddr_in serverAddress;
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(std::stoi(argv[1]));
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    if (bind(udpSocket, (struct sockaddr *)&serverAddress, sizeof(serverAddress)) < 0)
    {
        close(udpSocket);
        std::cerr << "error binding udp socket" << std::endl;
        return EXIT_FAILURE;
    }

    std::vector<Client> clients;
    std::thread receiveThread(&receive, std::ref(udpSocket), std::ref(clients));

    Sphere ball = initialBall;
    std::vector<Player> players = initialPlayers;

    constexpr uint FREQUENCY = 60;
    constexpr float PERIOD = 1.f / FREQUENCY;

    constexpr uint16_t GAME_DURATION = 300;
    constexpr uint16_t TRANSITION_DURATION = 5;
    constexpr uint8_t QUEUE_MIN = 1;
    constexpr uint8_t QUEUE_MAX = 10;
    constexpr uint8_t QUEUE_TARGET = (QUEUE_MIN + QUEUE_MAX) / 2;

    int64_t startTime = 0;
    int64_t transitionTime = 0;

    const auto period = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::duration<float>(PERIOD));
    auto targetTime = std::chrono::high_resolution_clock::now();
    // TODO: proper signal handling
    while (true)
    {
        const int64_t currentTime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();

        if (clients.size() == 2)
        {
            if (currentTime - startTime > GAME_DURATION)
            {
                startTime = currentTime + 5;
                transitionTime = currentTime;
                ball = initialBall;
                players[0].score = 0;
                players[1].score = 0;
            }
        }
        else
        {
            startTime = 0;
        }

        clients.erase(std::remove_if(clients.begin(), clients.end(),
                                     [](const Client &client)
                                     {
                                         return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - client.lastUpdate).count() > 5;
                                     }),
                      clients.end());

        std::vector<std::tuple<sockaddr_in *, size_t, uint32_t>> clientPlayerInputIds;
        for (auto &[address, queue, regulateQueue, playerId, _] : clients)
        {
            if (queue.size() < QUEUE_MIN || queue.size() > QUEUE_MAX)
            {
                regulateQueue = true;
            }
            else if (queue.size() == (QUEUE_MIN + QUEUE_MAX) / 2)
            {
                regulateQueue = false;
            }
            if (!regulateQueue || queue.size() > QUEUE_MAX)
            {
                const Input &input = queue.front();
                players[playerId].action = input.player.action;
                players[playerId].carState = input.player.carState;
                clientPlayerInputIds.push_back({&address, playerId, input.id});
                for (int i = 0; i < (regulateQueue ? queue.size() - QUEUE_TARGET : 1); i++)
                    queue.pop();
            }
        }

        for (const auto &[address, playerId, inputId] : clientPlayerInputIds)
        {
            const size_t otherPlayer = playerId ^ 1;
            int64_t countdown = GAME_DURATION - currentTime + startTime;
            int64_t transitionCountdown = TRANSITION_DURATION - currentTime + transitionTime;
            if (transitionCountdown < 0)
                transitionCountdown = 0;
            if (countdown < 0)
                countdown = 0;
            char buffer[102];
            std::memcpy(buffer, &inputId, 4);
            std::memcpy(buffer + 4, players[otherPlayer].carState.position.data(), 12);
            std::memcpy(buffer + 16, players[otherPlayer].carState.velocity.data(), 12);
            std::memcpy(buffer + 28, players[otherPlayer].carState.orientation.data(), 12);
            std::memcpy(buffer + 40, &players[otherPlayer].action.steering, 4);
            std::memcpy(buffer + 44, &players[otherPlayer].action.throttle, 4);
            std::memcpy(buffer + 48, ball.objectState.position.data(), 12);
            std::memcpy(buffer + 60, ball.objectState.velocity.data(), 12);
            std::memcpy(buffer + 72, ball.objectState.orientation.data(), 12);
            std::memcpy(buffer + 84, &countdown, 8);
            std::memcpy(buffer + 92, &transitionCountdown, 8);
            std::memcpy(buffer + 100, &players[0].score, 1);
            std::memcpy(buffer + 101, &players[1].score, 1);
            sendto(udpSocket, buffer, sizeof(buffer), 0, (sockaddr *)address, sizeof(sockaddr));
        }

        const uint8_t scores[2] = {players[0].score, players[1].score};
        physicsStep(arenaSize, goal, ball, carSize, players, true);
        if (players[0].score != scores[0] || players[1].score != scores[1])
        {
            transitionTime = currentTime;
            startTime += 5;
        }

        targetTime += period;
        std::this_thread::sleep_until(targetTime);
    }

    close(udpSocket);
    return EXIT_SUCCESS;
}
