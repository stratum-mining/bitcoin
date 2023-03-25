#include <consensus/merkle.h>
#include <netbase.h>
#include <sv2_template_provider.h>
#include <util/thread.h>
#include <validation.h>

void Sv2TemplateProvider::BindListenPort(uint16_t port)
{
    const CService addr_bind = LookupNumeric("0.0.0.0", port);

    std::unique_ptr<Sock> sock = CreateSock(addr_bind);
    if (!sock) {
        throw std::runtime_error("Sv2 Template Provider cannot create socket");
    }

    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);

    if (!addr_bind.GetSockAddr(reinterpret_cast<struct sockaddr*>(&sockaddr), &len)) {
        throw std::runtime_error("Sv2 Template Provider failed to get socket address");
    }

    if (sock->Bind(reinterpret_cast<struct sockaddr*>(&sockaddr), len) == SOCKET_ERROR) {
        const int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE) {
            throw std::runtime_error(strprintf("Unable to bind to %s on this computer. %s is probably already running.\n", addr_bind.ToString(), PACKAGE_NAME));
        }

        throw std::runtime_error(strprintf("Unable to bind to %s on this computer (bind returned error %s )\n", addr_bind.ToString(), NetworkErrorString(nErr)));
    }

    constexpr int max_pending_conns{4096};
    if (sock->Listen(max_pending_conns) == SOCKET_ERROR) {
        throw std::runtime_error("Sv2 Template Provider listening socket has an error");
    }

    m_listening_socket = std::move(sock);
    LogPrintf("Sv2 Template Provider listening on port: %d\n", port);
};

void Sv2TemplateProvider::Start(uint16_t port)
{
    // For now, not handling the BindListenPort method since if a port cannot be opened
    // for the template provider, the process should exit.
    BindListenPort(port);

    // Build and cache a block for the best new template ready for downstream connections.
    constexpr auto default_coinbase_tx_output_size {0};
    UpdateTemplate(true, default_coinbase_tx_output_size);

    // Update the best known previous hash for downstream connections.
    UpdatePrevHash();

    // Start the dedicated Stratum V2 handler thread.
    m_thread_sv2_handler = std::thread(&util::TraceThread, "sv2", [this] { ThreadSv2Handler(); });
};

void Sv2TemplateProvider::ThreadSv2Handler()
{
    while (!m_flag_interrupt_sv2) {
        if (m_chainman.ActiveChainstate().IsInitialBlockDownload()) {
            m_interrupt_sv2.sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        {
            // Required locking order for g_best_block_mutex.
            LOCK2(cs_main, m_mempool.cs);

            {
                WAIT_LOCK(g_best_block_mutex, lock);
                auto checktime = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                if (g_best_block_cv.wait_until(lock, checktime) == std::cv_status::timeout) {
                    if (m_best_prev_hash.m_prev_hash != g_best_block) {
                        // TODO: Maybe use default_coinbase_tx_output_size?
                        UpdateTemplate(true, 0);
                        UpdatePrevHash();
                        OnNewBlock();
                    }
                }
            }
        }

        // Remove clients that are flagged for disconnection.
        m_sv2_clients.erase(
                std::remove_if(m_sv2_clients.begin(), m_sv2_clients.end(), [](const Sv2Client &client) {
                    return client.m_disconnect_flag;

        }), m_sv2_clients.end());

        // Poll/Select the sockets that need handling.
        Sock::EventsPerSock events_per_sock = GenerateWaitSockets();

        constexpr auto timeout = std::chrono::milliseconds(50);
        // TODO: This has a bool that should be handled?
        // MAYBE If its false, continue?
        events_per_sock.begin()->first->WaitMany(timeout, events_per_sock);

        const auto listening_sock = events_per_sock.find(m_listening_socket);
        if (listening_sock != events_per_sock.end() && listening_sock->second.occurred & Sock::RECV) {
            struct sockaddr_storage sockaddr;
            socklen_t sockaddr_len = sizeof(sockaddr);

            auto sock = m_listening_socket->Accept(reinterpret_cast<struct sockaddr*>(&sockaddr), &sockaddr_len);
            // TODO: Maybe check if (sock) and if not just ignore??? Does this have any consequence for the actualy tcp connection on the socket?
            m_sv2_clients.emplace_back(Sv2Client{std::move(sock)});
        }

        for (auto& client : m_sv2_clients) {
            bool has_received_data = false;
            bool has_error_occurred = false;

            const auto it = events_per_sock.find(client.m_sock);
            if (it != events_per_sock.end()) {
                has_received_data = it->second.occurred & Sock::RECV;
                has_error_occurred = it->second.occurred & Sock::ERR;
            }

            if (has_error_occurred) {
                client.m_disconnect_flag = true;
            }

            if (has_received_data) {
                uint8_t bytes_received_buf[0x10000];
                auto num_bytes_received = client.m_sock->Recv(bytes_received_buf, sizeof(bytes_received_buf), MSG_DONTWAIT);

                if (num_bytes_received <= 0) {
                    client.m_disconnect_flag = true;
                    continue;
                }

                // TODO: This is required because it has the rest of the message bytes after reading the header.
                CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                ss << Span<uint8_t>(bytes_received_buf, num_bytes_received);

                Sv2Header sv2_header;
                try {
                    ss >> sv2_header;
                } catch (const std::exception& e) {
                    LogPrintf("Received invalid header: %s\n", e.what());
                    client.m_disconnect_flag = true;
                    continue;
                }

                // TODO: Maybe give it a better name than ss
                ProcessSv2Message(sv2_header, ss, client);
            }
        }
    }
}

void Sv2TemplateProvider::StopThreads()
{
    if (m_thread_sv2_handler.joinable()) {
        m_thread_sv2_handler.join();
    }
}

void Sv2TemplateProvider::Interrupt()
{
    m_flag_interrupt_sv2 = true;
}


void Sv2TemplateProvider::UpdatePrevHash()
{
    auto cached_block = m_blocks_cache.find(m_new_template.m_template_id);

    // TODO: Use the best new templates cached block to create the best new prev hash that
    // references that block?
    if (cached_block != m_blocks_cache.end()) {
        const CBlock block = cached_block->second->block;
        m_best_prev_hash = SetNewPrevHash{block, m_new_template.m_template_id};
    }
}

void Sv2TemplateProvider::UpdateTemplate(bool future, unsigned int out_data_size)
{
    node::BlockAssembler::Options options;
    options.nBlockMaxWeight = MAX_BLOCK_WEIGHT - out_data_size;
    options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);

    std::unique_ptr<node::CBlockTemplate> blocktemplate = node::BlockAssembler(m_chainman.ActiveChainstate(), &m_mempool, options).CreateNewBlock(CScript());

    uint64_t id = ++m_template_id;
    NewTemplate new_template{blocktemplate->block, id, future};
    m_blocks_cache.insert({new_template.m_template_id, std::move(blocktemplate)});
    m_new_template = new_template;
}

void Sv2TemplateProvider::OnNewBlock()
{
    for (const auto& client : m_sv2_clients) {
        if (!client.m_setup_connection_confirmed) {
            continue;
        }

        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);

        try {
            ss << Sv2NetMsg<NewTemplate>{Sv2MsgType::NEW_TEMPLATE, m_new_template};
        } catch (const std::exception& e) {
            LogPrintf("Error writing m_new_template: %e\n", e.what());
        }

        ssize_t sent = client.m_sock->Send(ss.data(), ss.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent != static_cast<ssize_t>(ss.size())) {
            LogPrintf("Failed to send\n");
        }
        ss.clear();

        try {
            ss << Sv2NetMsg<SetNewPrevHash>{Sv2MsgType::SET_NEW_PREV_HASH, m_best_prev_hash};
        } catch (const std::exception& e) {
            LogPrintf("Error writing m_best_prev_hash: %e\n", e.what());
        }

        sent = client.m_sock->Send(ss.data(), ss.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent != static_cast<ssize_t>(ss.size())) {
            LogPrintf("Failed to send\n");
        }
    }
}

void Sv2TemplateProvider::ProcessSv2Message(const Sv2Header& sv2_header, CDataStream& ss, Sv2Client& client)
{
    switch (sv2_header.m_msg_type) {
    case SETUP_CONNECTION: {
        if (client.m_setup_connection_confirmed) {
            return;
        }

        SetupConnection setup_conn;
        try {
            ss >> setup_conn;
        } catch (const std::exception& e) {
            LogPrintf("Received invalid SetupConnection message: %s\n", e.what());
            client.m_disconnect_flag = true;
            return;
        }

        if (setup_conn.m_protocol == SETUP_CONN_TP_PROTOCOL) {
            client.m_setup_connection_confirmed = true;

            CDataStream setup_success_ss(SER_NETWORK, PROTOCOL_VERSION);
            // TODO: Remove magic numbers.
            SetupConnectionSuccess setup_success{2, 0};
            setup_success_ss << Sv2NetMsg<SetupConnectionSuccess>{Sv2MsgType::SETUP_CONNECTION_SUCCESS, setup_success};

            ssize_t sent = client.m_sock->Send(setup_success_ss.data(), setup_success_ss.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent != static_cast<ssize_t>(setup_success_ss.size())) {
                LogPrintf("Failed to send\n");
            }
        }
        break;
    }
    case COINBASE_OUTPUT_DATA_SIZE: {
        if (!client.m_setup_connection_confirmed) {
            return;
        }

        CoinbaseOutputDataSize coinbase_out_data_size;
        try {
            ss >> coinbase_out_data_size;
            client.m_coinbase_output_data_size_recv = true;
        } catch (const std::exception& e) {
            LogPrintf("Received invalid CoinbaseOutputDataSize message: %s\n", e.what());
            return;
        }

        CDataStream new_prev_hash_ss(SER_NETWORK, PROTOCOL_VERSION);
        try {
            new_prev_hash_ss << Sv2NetMsg<SetNewPrevHash>{Sv2MsgType::SET_NEW_PREV_HASH, m_best_prev_hash};
        } catch (const std::exception& e) {
            LogPrintf("Error writing prev_hash: %e\n", e.what());
        }

        ssize_t sent = client.m_sock->Send(new_prev_hash_ss.data(), new_prev_hash_ss.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent != static_cast<ssize_t>(new_prev_hash_ss.size())) {
            LogPrintf("Failed to send\n");
        }

        client.m_coinbase_tx_outputs_size = coinbase_out_data_size.m_coinbase_output_max_additional_size;
        UpdateTemplate(true, client.m_coinbase_tx_outputs_size);

        CDataStream new_template_ss(SER_NETWORK, PROTOCOL_VERSION);
        try {
            new_template_ss << Sv2NetMsg<NewTemplate>{Sv2MsgType::NEW_TEMPLATE, m_new_template};
        } catch (const std::exception& e) {
            LogPrintf("Error writing copy_new_template: %e\n", e.what());
        }

        sent = client.m_sock->Send(new_template_ss.data(), new_template_ss.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent != static_cast<ssize_t>(ss.size())) {
            LogPrintf("Failed to send\n");
        }

        break;
    }
    case SUBMIT_SOLUTION: {
        SubmitSolution submit_solution;
        try {
            ss >> submit_solution;
        } catch (const std::exception& e) {
            LogPrintf("Received invalid SubmitSolution message: %e\n", e.what());
            return;
        }

        auto cached_block = m_blocks_cache.find(submit_solution.m_template_id);
        if (cached_block != m_blocks_cache.end()) {
            auto block_template = *cached_block->second;
            CBlock& block = block_template.block;

            // TODO: Can't I just move this in one go?
            auto coinbase_tx = CTransaction(std::move(submit_solution.m_coinbase_tx));
            block.vtx[0] = std::make_shared<CTransaction>(std::move(coinbase_tx));

            block.nVersion = submit_solution.m_version;
            block.nTime = submit_solution.m_header_timestamp;
            block.nNonce = submit_solution.m_header_nonce;
            block.hashMerkleRoot = BlockMerkleRoot(block);

            auto blockptr = std::make_shared<CBlock>(std::move(block));

            bool new_block{true};
            bool res = m_chainman.ProcessNewBlock(blockptr, true /* force_processing */, true /* min_pow_checked */, &new_block);
            if (res) {
                m_blocks_cache.erase(submit_solution.m_template_id);

                UpdateTemplate(true, client.m_coinbase_tx_outputs_size);
                UpdatePrevHash();

                OnNewBlock();
            }
        }
        break;
    }
    default: {
        break;
    }
    }
}

Sock::EventsPerSock Sv2TemplateProvider::GenerateWaitSockets() const
{
    Sock::EventsPerSock events_per_sock;
    events_per_sock.emplace(m_listening_socket, Sock::Events(Sock::RECV));

    for (const auto& sv2_client : m_sv2_clients) {
        // TODO: Also check if each nodes m_sock is some?
        if (!sv2_client.m_disconnect_flag) {
            events_per_sock.emplace(sv2_client.m_sock, Sock::Events{Sock::RECV | Sock::ERR});
        }
    }

    return events_per_sock;
}
