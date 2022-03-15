// TMP
#include <netbase.h>
#include <util/thread.h>
#include <util/syscall_sandbox.h>
#include <sv2_distributor.h>

// TMP: TO SIMULATE POW
#include <pow.h>
#include <consensus/merkle.h>
/* #include <rusty/protocols/v2/sv2-ffi/sv2.h> */
// TMP

#include <streams.h>
#include <uint256.h>
#include <time.h>
#include <validation.h>
#include <miner.h>


CNewTemplate Sv2Distributor::AssembleSv2BlockTemplate(const CBlock& block)
{
    CNewTemplate ctemplate;
    ctemplate.template_id = GetTimeSeconds();

    // TODO: Decide when this is a future block or not.
    ctemplate.future_template = false;
    ctemplate.version = block.GetBlockHeader().nVersion;

    const auto coinbase_tx = block.vtx[0];
    ctemplate.coinbase_tx_version = coinbase_tx->CURRENT_VERSION;

    CDataStream coinbase_script(SER_NETWORK, PROTOCOL_VERSION);
    coinbase_script << coinbase_tx->vin[0].scriptSig;

    // TODO: Double check why 8 is hardcoded as the length? Also is it just the first 8 bytes of the scriptSig? Which is NOT the length and block ehight, so might need ot bit shift or access the correct index.
    ctemplate.coinbase_prefix = cvec_from_buffer(&coinbase_script[0], 8);
    ctemplate.coinbase_tx_input_sequence = coinbase_tx->vin[0].nSequence;

    // TODO: Can keep this set to 0, since this will be modified by the client?
    ctemplate.coinbase_tx_value_remaining = 0;

    // TODO: If this is empty, then should be encoded as 00 and the coinbase_tx_outputs should be an empty vec![];
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
        // TODO: Need to configure port according to import
        CService addrBind = LookupNumeric("0.0.0.0", 8442);
        std::unique_ptr<Sock> sock = CreateSock(addrBind);
        if (!sock) {
            LogPrintf("SV2DEBUG: Error opening socket\n");
            return;
        }

        struct sockaddr_storage sockaddr;
        socklen_t len = sizeof(sockaddr);

        if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len)) {
            LogPrintf("SV2DEBUG: Error: Bind address family for %s not supported", addrBind.ToString());
            return;
        }

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
        SetSyscallSandboxPolicy(SyscallSandboxPolicy::NET);

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(8442); // TODO: tmp port number
        int addrlen = sizeof(address);

        EncoderWrapper* encoder = new_encoder();

        while (!m_flag_interrupt_sv2) {
            // TODO: Tmp state to block any processing until IBD is complete
            if (m_chainman.ActiveChainstate().IsInitialBlockDownload()) {
                m_interrupt_sv2.sleep_for(std::chrono::milliseconds(500));
                LogPrintf("DEBUG: Chainstate is in IBD\n");
                continue;
            }

            std::set<SOCKET> recv_set, send_set, err_set;
            std::set<SOCKET> recv_select_set, send_select_set, err_select_set;

            recv_select_set.insert(m_listening_socket->Get());

            for (const Sv2Client& client : m_sv2_clients) {
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

            select(hSocketMax + 1, &fdsetRecv, &fdsetSend, &fdsetError, &timeout);

            // TODO: Check each fd in the sets if select is activated? and add them to the the recv_sets.
            for (int hSocket : recv_select_set) {
                if (FD_ISSET(hSocket, &fdsetRecv)) {
                    recv_set.insert(hSocket);
                }
            }

            for (int hSocket : send_select_set) {
                /* if (FD_ISSET(hSocket, &fdsetSend)) { */
                /*     send_set.insert(hSocket); */
                /* } */
            }

            for (int hSocket : err_select_set) {
                if (FD_ISSET(hSocket, &fdsetError)) {
                    err_set.insert(hSocket);
                }
            }

            if (m_listening_socket->Get() != INVALID_SOCKET && recv_set.count(m_listening_socket->Get()) > 0) {
                SOCKET hSocket = accept(m_listening_socket->Get(), (struct sockaddr*)&address, (socklen_t*)&addrlen);

                std::unique_ptr<Sock> sock = std::make_unique<Sock>(hSocket);
                Sv2Client client = Sv2Client(std::move(sock));
                m_sv2_clients.push_back(std::move(client));
            }

            for (Sv2Client& client : m_sv2_clients) {
                /* LogPrintf("CCDLE12: Iterating over client: %d\n", client.m_sock->Get()); */
                bool recvSet = false;
                bool sendSet = false;
                bool errSet = false;

                recvSet = recv_set.count(client.m_sock->Get()) > 0;
                sendSet = send_set.count(client.m_sock->Get()) > 0;
                errSet = err_set.count(client.m_sock->Get()) > 0;

                DecoderWrapper* decoder = new_decoder();
                bool f = true;
                int cursor = 0;
                if (recvSet) {
                    uint8_t pchBuf[0x10000];
                    int bytes_read = recv(client.m_sock->Get(), (char*)pchBuf, sizeof(pchBuf), MSG_DONTWAIT);

                    // TODO: Rename the variable for this while loop.
                    while (f) {
                      CVec buffer = get_writable(decoder);

                      if (bytes_read < buffer.len || cursor > bytes_read) {
                          f = false;
                          continue;
                      }

                      LogPrintf("CCDLE12: Buffer.len: %d\n", buffer.len);
                      if (bytes_read > 0) {
                          LogPrintf("CCDLE12: cursor: %d\n", cursor);
                            // NOTE: Copying the pchBuffer into the buffer.data accordoing ot length, this will allow next_frame to parse the message.
                            memcpy(buffer.data, &pchBuf[cursor], buffer.len);

                            LogPrintf("CCDLE12: before next frame\n");
                            CResult<CSv2Message, Sv2Error> frame = next_frame(decoder);
                            LogPrintf("CCDLE12: after next frame\n");

                            switch (frame.tag) {
                              case CResult<CSv2Message, Sv2Error>::Tag::Ok: {
                                  LogPrintf("CCDLE12: Tag::Ok received\n");
                                  switch (frame.ok._0.tag) {
                                      case CSv2Message::Tag::SetupConnection:
                                        {
                                          if (client.m_setup_connection_confirmed) {
                                              // TODO: Maybe we drop the connection?
                                              f = false;
                                              break;
                                          }

                                         // TODO: Can this be a pointer?
                                         CSetupConnection setup_connection = frame.ok._0.setup_connection._0;

                                          // TODO: Switch on the CSv2Message protocol, it should ONLY be templatedistribution
                                         switch (setup_connection.protocol) {
                                             case Protocol::TemplateDistributionProtocol:
                                               {
                                                   LogPrintf("CCDLE12: Received a SetupConnection TemplateDistributionProtocol\n");

                                                   uint16_t min_version = setup_connection.min_version;
                                                   LogPrintf("CCDLE12: min_version from the setupconnection message: %d\n", min_version);

                                                   client.m_setup_connection_confirmed = true;
                                                    
                                                   // TODO: Respond the setupconnection success?
                                                   // TODO: Send the SetupConnectionSuccess and figure out what to assign to used_version and flags.
                                                   // TODO: Maybe wrap this in a helper function to encode a message?
                                                   SetupConnectionSuccess setup_connection_success;
                                                   setup_connection_success.used_version = 2; 
                                                   setup_connection_success.flags = 0;
                                                   LogPrintf("CCDLE12 DEBUG\n");

                                                   CSv2Message response;
                                                   response.tag = CSv2Message::Tag::SetupConnectionSuccess;
                                                   response.setup_connection_success._0 = setup_connection_success;

                                                   CResult<CVec, Sv2Error> encoded = encode(&response, encoder);
                                                   LogPrintf("CCDLE12 DEBUG\n");

                                                   switch (encoded.tag) {
                                                       case CResult < CVec, Sv2Error > ::Tag::Ok:
                                                         write(client.m_sock->Get(), encoded.ok._0.data, encoded.ok._0.len);
                                                         break;
                                                       case CResult < CVec, Sv2Error > ::Tag::Err:
                                                         // TODO: What do we do?
                                                         break;
                                                   }
                                                   free_encoder(encoder);
                                                    
                                                  // TODO: CCDLE12 SV2 - tmp build a block and serialize it out over the fd.
                                                  // TODO: CDDLE12 SV2 - Build a block separately and pass it in as a reference to a blocktemplate
                                                  BlockAssembler::Options options;
                                                  options.nBlockMaxWeight = MAX_BLOCK_WEIGHT;
                                                  options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);

                                                  /* std::unique_ptr<CBlockTemplate> blocktemplate = BlockAssembler(m_chainstate, m_mempool, m_chainparams, options).CreateNewBlock(CScript()); */
                                                  std::unique_ptr<CBlockTemplate> blocktemplate = BlockAssembler(m_chainman.ActiveChainstate(), m_mempool, m_chainparams, options).CreateNewBlock(CScript());

                                                  // TODO: Rename to BuildSv2BlockTemplate
                                                  CNewTemplate sv2_new_template = AssembleSv2BlockTemplate(blocktemplate->block);

                                                  // TODO: Sections - Build the message on the wire using the CNewTemplate message.
                                                  CSv2Message new_template_response;
                                                  new_template_response.tag = CSv2Message::Tag::NewTemplate;
                                                  new_template_response.new_template._0 = sv2_new_template;

                                                  CResult<CVec, Sv2Error> new_template_encoded = encode(&new_template_response, encoder);

                                                  switch (new_template_encoded.tag) {
                                                      case CResult < CVec, Sv2Error > ::Tag::Ok:
                                                        LogPrintf("CCDLE12 DEBUG: BEFORE WRITING NEW TEMPLATE\n");
                                                        write(client.m_sock->Get(), new_template_encoded.ok._0.data, new_template_encoded.ok._0.len);
                                                        LogPrintf("CCDLE12 DEBUG: AFTER WRITING NEW TEMPLATE\n");
                                                        break;
                                                      case CResult < CVec, Sv2Error > ::Tag::Err:
                                                        LogPrintf("CCDLE12 DEBUG: encoding error for new_template\n");
                                                        break;
                                                  }
                                                  free_encoder(encoder);

                                                  // TODO: Maybe just send a new new template hash here? I dont know why but I'll just do it anyway
                                                  // TODO: Can I reuse the encoder?
                                                  CSetNewPrevHash set_new_prev_hash;
                                                  set_new_prev_hash.template_id = sv2_new_template.template_id;
                                                  set_new_prev_hash.prev_hash = cvec_from_buffer(blocktemplate->block.hashPrevBlock.data(), blocktemplate->block.hashPrevBlock.size());
                                                  set_new_prev_hash.header_timestamp = blocktemplate->block.nTime;
                                                  set_new_prev_hash.n_bits = blocktemplate->block.nBits;

                                                  // TODO: Not sure if this is the correct value to use for target.
                                                  uint256 target = ArithToUint256(arith_uint256().SetCompact(blocktemplate->block.nBits));
                                                  set_new_prev_hash.target = cvec_from_buffer(target.data(), target.size());

                                                  CSv2Message set_new_prev_hash_msg;
                                                  set_new_prev_hash_msg.tag = CSv2Message::Tag::SetNewPrevHash;
                                                  set_new_prev_hash_msg.set_new_prev_hash._0 = set_new_prev_hash;

                                                  CResult<CVec, Sv2Error> new_prev_hash_encoded = encode(&set_new_prev_hash_msg, encoder);

                                                  switch (new_prev_hash_encoded.tag) {
                                                      case CResult<CVec, Sv2Error>::Tag::Ok:
                                                        LogPrintf("CCDLE12 DEBUG: BEFORE WRITING new prev hash\n");
                                                        write(client.m_sock->Get(), new_prev_hash_encoded.ok._0.data, new_prev_hash_encoded.ok._0.len);
                                                        LogPrintf("CCDLE12 DEBUG: AFTER WRITING new prev hash\n");
                                                        break;
                                                      case CResult<CVec, Sv2Error>::Tag::Err:
                                                        break;
                                                  }
                                                  free_encoder(encoder);

                                                  f = false;
                                                  break;
                                               }
                                             // TODO: CCDLE12 If the message is NOT TemplateDistributionProtocol then break and drop connection
                                             default:
                                               { 
                                                   LogPrintf("CCDLE12: Different protocol received\n");
                                                   f = false;
                                                   break;
                                               }       
                                         }
                                         break;
                                       }
                                      case CSv2Message::Tag::SubmitSolution:
                                         {
                                             LogPrintf("CCDLE12: Received a submit solution\n");

                                             // TODO: CCDLE12 SV2 - tmp build a block and serialize it out over the fd.
                                             // TODO: CDDLE12 SV2 - Build a block separately and pass it in as a reference to a blocktemplate
                                             BlockAssembler::Options options;
                                             options.nBlockMaxWeight = MAX_BLOCK_WEIGHT;
                                             options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);

                                             // TMP: Create a block for now, later use the block that was created and sent downstream.
                                             std::unique_ptr<CBlockTemplate> blocktemplate = BlockAssembler(m_chainman.ActiveChainstate(), m_mempool, m_chainparams, options).CreateNewBlock(CScript());

                                             LogPrintf("CCDLE12: Calling ProcessNewBlock()\n");
                                             std::shared_ptr<CBlock> blockptr = std::make_shared<CBlock>(blocktemplate->block);

                                             // TODO: We calculate the merkleroot before performing the POW. Need to send the merkle root in the SV2 Template?
                                             bool mutated{true};
                                             blockptr->hashMerkleRoot = BlockMerkleRoot(*blockptr, &mutated);
                                             LogPrintf("CCDLE12: tx size in blockptr: %s\n", blockptr->vtx.size());
                                             LogPrintf("CCDLE12: merkroot of blockptr: %s\n", blockptr->hashMerkleRoot.GetHex());

                                             // TMP: To get the POW to pass.
                                             // Maybe do this at the client side?
                                             LogPrintf("CCDLE12: nBits of the block: %d\n", blockptr->nBits);
                                             LogPrintf("CCDLE12: nNonce of the block: %d\n", blockptr->nNonce);
                                             while (!CheckProofOfWork(blockptr->GetHash(), blockptr->nBits, m_chainparams.GetConsensus())) {
                                                 LogPrintf("CCDLE12: Incrementing the nNonce until the proof of works passes\n");
                                                 ++(blockptr->nNonce);
                                             }
                                             LogPrintf("CCDLE12: nNonce of the block AFTER: %d\n", blockptr->nNonce);

                                             // TODO: This will send the block for processing, validation and sending out over the P2P network.
                                             bool new_block{true};
                                             bool res = m_chainman.ProcessNewBlock(m_chainparams, blockptr, true /* force_processing */, &new_block /* new_block */);
                                             LogPrintf("CCDLE12: AFTER Calling ProcessNewBlock(): Result of ProcessNewBlock(): %d\n", res);

                                             f = false;
                                             break;
                                         }

                                      break;
                                  } // frame.ok._0.tag
                                  break;
                              }
                              case CResult<CSv2Message, Sv2Error>::Tag::Err:
                                {
                                LogPrintf("CCDLE12: Error in tag received\n");
                                // NOTE: Increase the cursor by the buffer.len, this allows the cursor to move the next bytes in pchBuf.
                                cursor += buffer.len;
                                /* cursor += 6; */
                                break;
                                }
                            }; //switch frame.tag
                      } // bytes_read
                      else { 
                        LogPrintf("CCDLE12: 0 bytes read\n");
                        f = false; 
                      };
                    } // while (f)
                } // recv_set
            } // for loop
        } // main while loop
} // ThreadSv2Handler

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
