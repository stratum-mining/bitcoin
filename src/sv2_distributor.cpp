// TMP
#include <netbase.h>
#include <util/thread.h>
#include <util/syscall_sandbox.h>
#include <sv2_distributor.h>
/* #include <rusty/protocols/v2/sv2-ffi/sv2.h> */
// TMP

#include <streams.h>
#include <uint256.h>
#include <time.h>
#include <validation.h>
#include <miner.h>


CNewTemplate Sv2Distributor::AssembleSv2BlockTemplate()
{
    BlockAssembler::Options options;
    options.nBlockMaxWeight = MAX_BLOCK_WEIGHT;
    options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);

    std::unique_ptr<CBlockTemplate> pblocktemplate = BlockAssembler(m_chainstate, m_mempool, m_chainparams, options)
        .CreateNewBlock(CScript());

    const CBlock block = pblocktemplate->block;
    const CBlockHeader header = block.GetBlockHeader();
    
    CNewTemplate ctemplate;
    ctemplate.template_id = GetTimeSeconds();

    // TODO: Decide when this is a future block or not.
    ctemplate.future_template = false;
    ctemplate.version = header.nVersion;

    const auto coinbase_tx = block.vtx[0];
    ctemplate.coinbase_tx_version = coinbase_tx->CURRENT_VERSION;

    CDataStream coinbase_script(SER_NETWORK, PROTOCOL_VERSION);
    coinbase_script << coinbase_tx->vin[0].scriptSig;

    // TODO: Double check why 8 is hardcoded as the length?
    ctemplate.coinbase_prefix = cvec_from_buffer(&coinbase_script[0], 8);
    ctemplate.coinbase_tx_input_sequence = coinbase_tx->vin[0].nSequence;

    // TODO: Can keep this set to 0, since this will be modified by the client?
    ctemplate.coinbase_tx_value_remaining = 0;
    ctemplate.coinbase_tx_outputs_count = coinbase_tx->vout.size();
    
    CDataStream vout(SER_NETWORK, PROTOCOL_VERSION);
    vout << coinbase_tx->vout;
    ctemplate.coinbase_tx_outputs = cvec_from_buffer(&vout[0], vout.size());
    ctemplate.coinbase_tx_locktime = coinbase_tx->nLockTime;

    CVec2 cvec2 = init_cvec2();
    for (const auto& tx: block.vtx) {
        CDataStream merkle_path_stream(SER_NETWORK, PROTOCOL_VERSION);
        merkle_path_stream << tx->GetHash();

        auto merkle_path = cvec_from_buffer(&merkle_path_stream[0], merkle_path_stream.size());
        cvec2_push(&cvec2, merkle_path);
    }

    ctemplate.merkle_path = cvec2;

    return ctemplate;
}

void Sv2Distributor::BindListenPort()
{
        CService addrBind = LookupNumeric("0.0.0.0", 8442);
        std::unique_ptr<Sock> sock = CreateSock(addrBind);
        if (!sock) {
            LogPrintf("SV2DEBUG: Error opening socket\n");
            return;
        }

        // TODO:
        // What is sockaddr_storage?
        struct sockaddr_storage sockaddr;
        socklen_t len = sizeof(sockaddr);

        // TODO: Manipulates the addr to the correct format for the OS
        if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len)) {
            LogPrintf("SV2DEBUG: Error: Bind address family for %s not supported", addrBind.ToString());
            return;
        }

        // TODO: Now actually bind the port?
        if (bind(sock->Get(), (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR) {
            int nErr = WSAGetLastError();
            if (nErr == WSAEADDRINUSE)
                LogPrintf("SV2DEBUG: Unable to bind to %s on this computer. %s is probably already running.\n", addrBind.ToString(), PACKAGE_NAME);
            else
                LogPrintf("SV2DEBUG: Unable to bind to %s on this computer (bind returned error %s)\n", addrBind.ToString(), NetworkErrorString(nErr));
            return;
        }
        LogPrintf("SV2DEBUG: Bound to %s\n", addrBind.ToString());

        // TODO: Need to handle this better BUT actually we could extract some
        // of Connman to netbase? This uses the exact same logic
        /* if ((m_listening_socket = listen(sock->Get(), 4096)) == SOCKET_ERROR) { */
        if ((listen(sock->Get(), 4096)) == SOCKET_ERROR) {
            LogPrintf("SV2DEBUG: Error listening to port\n");
            return;
        }

        m_listening_socket = std::move(sock);
        LogPrintf("SV2DEBUG: Completed BindListenPort\n");
        return; 
};

void Sv2Distributor::ThreadSv2Handler() 
{
        // TODO: I think this limits any calls within this thread/function? to
        // limit the syscalls
        SetSyscallSandboxPolicy(SyscallSandboxPolicy::NET);

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(8442); // TODO: tmp port number
        int addrlen = sizeof(address);

        while (!m_flag_interrupt_sv2) {
            /* LogPrintf("SV2DEBUG: start of loop\n"); */
            std::set<SOCKET> recv_set, send_set, err_set;
            std::set<SOCKET> recv_select_set, send_select_set, err_select_set;

            recv_select_set.insert(m_listening_socket->Get());

            for (const Sv2Client& client : m_sv2_clients) {
                /* LogPrintf("SV2DEBUG: Adding connection from fd: %d\n", client.m_sock); */
                recv_select_set.insert(client.m_sock->Get());
                send_select_set.insert(client.m_sock->Get());
                err_select_set.insert(client.m_sock->Get());
            }

            fd_set fdsetRecv;
            fd_set fdsetSend;
            fd_set fdsetError;
            FD_ZERO(&fdsetRecv);
            FD_ZERO(&fdsetSend);
            FD_ZERO(&fdsetError);
            int hSocketMax = 0;

            for (int hSocket : recv_select_set) {
                FD_SET(hSocket, &fdsetRecv);
                hSocketMax = std::max(hSocketMax, hSocket);
            }
           
            for (int hSocket : send_select_set) {
                FD_SET(hSocket, &fdsetSend);
                hSocketMax = std::max(hSocketMax, hSocket);
            }

            for (int hSocket : err_select_set) {
                FD_SET(hSocket, &fdsetError);
                hSocketMax = std::max(hSocketMax, hSocket);
            }

            struct timeval timeout;
            timeout.tv_sec  = 0;
            timeout.tv_usec = 50 * 1000; // frequency to call select

            /* int nSelect = select(hSocketMax + 1, &fdsetRecV, &fdsetSend, &fdsetError, &timeout); */
            select(hSocketMax + 1, &fdsetRecv, &fdsetSend, &fdsetError, &timeout);

            // TODO: Check each fd in the sets if select is activated? and add them to the the recv_sets.
            for (int hSocket : recv_select_set) {
                if (FD_ISSET(hSocket, &fdsetRecv)) {
                    recv_set.insert(hSocket);
                }
            }

            for (int hSocket : send_select_set) {
                if (FD_ISSET(hSocket, &fdsetSend)) {
                    send_set.insert(hSocket);
                }
            }

            for (int hSocket : err_select_set) {
                if (FD_ISSET(hSocket, &fdsetError)) {
                    err_set.insert(hSocket);
                }
            }

            if (m_listening_socket->Get() != INVALID_SOCKET && recv_set.count(m_listening_socket->Get()) > 0) {
                SOCKET hSocket = accept(m_listening_socket->Get(), (struct sockaddr*)&address, (socklen_t *)&addrlen);
                /* LogPrintf("SV2DEBUG: Accepted connection from fd: %d\n", hSocket); */

                // TODO: Create a client using this socket.
                std::unique_ptr<Sock> sock = std::make_unique<Sock>(hSocket);
                Sv2Client client = Sv2Client(std::move(sock));
                m_sv2_clients.push_back(std::move(client));
            }
            
            // TODO: Service each node
            for (const Sv2Client& client : m_sv2_clients) {
                bool recvSet = false;
                /* bool sendSet = false; */
                /* bool errSet = false; */

                recvSet = recv_set.count(client.m_sock->Get()) > 0;
                /* sendSet = send_set.count(client.m_sock) > 0; */
                /* errSet = err_set.count(client.m_sock) > 0; */

                /* LogPrintf("SV2DEBUG: looping through m_sv2_clients: %d\n", client.m_sock); */
                uint8_t pchBuf[0x10000];
                int nBytes = 0;
                if (recvSet) {
                    /* LogPrintf("SV2DEBUG: sock is in recv set: %d\n", client.m_sock); */
                    nBytes = recv(client.m_sock->Get(), (char*)pchBuf, sizeof(pchBuf), MSG_DONTWAIT);
                }

                if (nBytes > 0) {
                    printf("SV2DEBUG: received a message from: %d\n", client.m_sock->Get());
                    // TODO: CCDLE12 SV2 - tmp build a block and serialize it out over the fd.
                    auto blocktemplate = AssembleSv2BlockTemplate();

                    // TODO: Sections - Build the message on the wire using the CNewTemplATE message.
                    CSv2Message message;
                    message.tag = CSv2Message::Tag::NewTemplate;
                    message.new_template._0 = blocktemplate;

                    // TODO: Sections - Encoding section.
                    EncoderWrapper* encoder = new_encoder();
                    CResult<CVec, Sv2Error> encoded = encode(&message, encoder);

                    switch (encoded.tag) {
                    case CResult < CVec, Sv2Error > ::Tag::Ok:
                        printf("SV2DEBUG: Encoding OK\n");
                        break;
                    case CResult < CVec, Sv2Error > ::Tag::Err:
                        printf("SV2DEBUG: Encoding ERR\n");
                        break;
                    }

                    write(client.m_sock->Get(), encoded.ok._0.data, encoded.ok._0.len);
                    free_encoder(encoder);
                }
            }
        }
};

void Sv2Distributor::Start()
{
    m_thread_sv2_handler = std::thread(&util::TraceThread, "sv2", [this] { ThreadSv2Handler(); });
};

// TODO: More considerations when interrupt occurs other than just the thread being joinable
void Sv2Distributor::Interrupt()
{
    m_flag_interrupt_sv2 = true;
}

// TODO: Stop threads function.
void Sv2Distributor::StopThreads()
{
    if (m_thread_sv2_handler.joinable()) {
        m_thread_sv2_handler.join();
    }
}
