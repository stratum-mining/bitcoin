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

                Sv2NetHeader sv2_header;
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
    auto cached_block = m_blocks_cache.find(m_best_new_template.m_template_id);

    // TODO: Use the best new templates cached block to create the best new prev hash that
    // references that block?
    if (cached_block != m_blocks_cache.end()) {
        const CBlock block = cached_block->second->block;
        m_best_prev_hash = SetNewPrevHashMsg{block, m_best_new_template.m_template_id};
    }
}

void Sv2TemplateProvider::UpdateTemplate(bool future, unsigned int out_data_size)
{
    node::BlockAssembler::Options options;
    options.nBlockMaxWeight = MAX_BLOCK_WEIGHT - out_data_size;
    options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);

    std::unique_ptr<node::CBlockTemplate> blocktemplate = node::BlockAssembler(m_chainman.ActiveChainstate(), &m_mempool, options).CreateNewBlock(CScript());

    uint64_t id = ++m_template_id;
    NewTemplateMsg new_template{blocktemplate->block, id, future};
    m_blocks_cache.insert({new_template.m_template_id, std::move(blocktemplate)});
    m_best_new_template = new_template;
}

void Sv2TemplateProvider::OnNewBlock()
{
    CDataStream new_template_ss(SER_NETWORK, PROTOCOL_VERSION);
    try {
        new_template_ss << Sv2NetMsg{m_best_new_template};
    } catch (const std::exception& e) {
        LogPrintf("Error serializing m_best_new_template: %e\n", e.what());
        return;
    }

    CDataStream new_prev_hash_ss(SER_NETWORK, PROTOCOL_VERSION);
    try {
        new_prev_hash_ss << Sv2NetMsg{m_best_prev_hash};
    } catch (const std::exception& e) {
        LogPrintf("Error writing m_best_prev_hash: %e\n", e.what());
        return;
    }

    for (const auto& client : m_sv2_clients) {
        if (!client.m_setup_connection_confirmed) {
            continue;
        }

        try {
            ssize_t sent = client.m_sock->Send(new_template_ss.data(), new_template_ss.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent != static_cast<ssize_t>(new_template_ss.size())) {
                LogPrintf("Failed to send NewTemplate message\n");
                continue;
            }
        } catch (const std::exception& e) {
            LogPrintf("Error when sending NewTemplate message: %e\n", e.what());
            continue;
        }

        try {
            ssize_t sent = client.m_sock->Send(new_prev_hash_ss.data(), new_prev_hash_ss.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent != static_cast<ssize_t>(new_prev_hash_ss.size())) {
                LogPrintf("Failed to send SetNewPrevHash\n");
                continue;
            }
        } catch (const std::exception& e) {
            LogPrintf("Error when sending NewTemplate message: %e\n", e.what());
            continue;
        }
    }
}

void Sv2TemplateProvider::ProcessSv2Message(const Sv2NetHeader& sv2_header, CDataStream& ss, Sv2Client& client)
{
    switch (sv2_header.m_msg_type) {
    case SETUP_CONNECTION: {
        if (client.m_setup_connection_confirmed) {
            return;
        }

        SetupConnectionMsg setup_conn;
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

            // TODO: Move these to .h file scope?
            constexpr auto default_used_version{2};
            constexpr auto default_optional_feature_flags{0};

            SetupConnectionSuccessMsg setup_success{default_used_version, default_optional_feature_flags};
            setup_success_ss << Sv2NetMsg{setup_success};

            try { 
                ssize_t sent = client.m_sock->Send(setup_success_ss.data(), setup_success_ss.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
                if (sent != static_cast<ssize_t>(setup_success_ss.size())) {
                    LogPrintf("Failed to send SetupSuccessMessage\n");
                }
            } catch (const std::exception& e) {
                LogPrintf("Error when sending SetupSuccess message: %e\n", e.what());
            }
        }
        break;
    }
    case COINBASE_OUTPUT_DATA_SIZE: {
        if (!client.m_setup_connection_confirmed) {
            return;
        }

        CoinbaseOutputDataSizeMsg coinbase_output_data_size;
        try {
            ss >> coinbase_output_data_size;
            client.m_coinbase_output_data_size_recv = true;
        } catch (const std::exception& e) {
            LogPrintf("Received invalid CoinbaseOutputDataSize message: %s\n", e.what());
            return;
        }

        CDataStream new_prev_hash_ss(SER_NETWORK, PROTOCOL_VERSION);
        try {
            new_prev_hash_ss << Sv2NetMsg{m_best_prev_hash};
        } catch (const std::exception& e) {
            LogPrintf("Error writing prev_hash: %e\n", e.what());
        }

        try {
            ssize_t sent = client.m_sock->Send(new_prev_hash_ss.data(), new_prev_hash_ss.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent != static_cast<ssize_t>(new_prev_hash_ss.size())) {
                LogPrintf("Failed to send NewPrevHash message\n");
            }
        } catch (const std::exception& e) {
            LogPrintf("Error when sending NewPrevHash message: %e\n", e.what());
        }

        client.m_coinbase_tx_outputs_size = coinbase_output_data_size.m_coinbase_output_max_additional_size;
        UpdateTemplate(true, client.m_coinbase_tx_outputs_size);

        CDataStream new_template_ss(SER_NETWORK, PROTOCOL_VERSION);
        try {
            new_template_ss << Sv2NetMsg{m_best_new_template};
        } catch (const std::exception& e) {
            LogPrintf("Error writing copy_new_template: %e\n", e.what());
        }

        try {
            ssize_t sent = client.m_sock->Send(new_template_ss.data(), new_template_ss.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (sent != static_cast<ssize_t>(new_template_ss.size())) {
                LogPrintf("Failed to send NewTemplate Message,\n");
            }
        } catch (const std::exception& e) {
            LogPrintf("Error when sending NewTemplate message: %e\n", e.what());
        }

        break;
    }
    case SUBMIT_SOLUTION: {
        SubmitSolutionMsg submit_solution;
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


NewTemplateMsg::NewTemplateMsg(const CBlock& block, uint64_t template_id, bool future_template)
    : m_template_id{template_id}, m_future_template{future_template}
{
    m_version = block.GetBlockHeader().nVersion;

    const CTransactionRef coinbase_tx = block.vtx[0];
    m_coinbase_tx_version = coinbase_tx->CURRENT_VERSION;
    m_coinbase_prefix = coinbase_tx->vin[0].scriptSig;
    m_coinbase_tx_input_sequence = coinbase_tx->vin[0].nSequence;

    // The coinbase nValue already contains the nFee + the Block Subsidy when built using CreateBlock().
    m_coinbase_tx_value_remaining = static_cast<uint64_t>(block.vtx[0]->vout[0].nValue);

    m_coinbase_tx_outputs_count = 0;
    int commitpos = GetWitnessCommitmentIndex(block);
    if (commitpos != NO_WITNESS_COMMITMENT) {
        m_coinbase_tx_outputs_count = 1;

        std::vector<CTxOut> coinbase_tx_outputs{block.vtx[0]->vout[commitpos]};
        m_coinbase_tx_outputs = coinbase_tx_outputs;
    }

    m_coinbase_tx_locktime = coinbase_tx->nLockTime;

    // Skip the coinbase_tx hash from the merkle path, as the downstream client
    // will build their own coinbase tx.
    for (auto it = block.vtx.begin() + 1; it != block.vtx.end(); ++it) {
        m_merkle_path.push_back((*it)->GetHash());
    }
};
