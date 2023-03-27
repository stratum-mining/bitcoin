#ifndef BITCOIN_SV2_TEMPLATE_PROVIDER_H
#define BITCOIN_SV2_TEMPLATE_PROVIDER_H

#include <arith_uint256.h>
#include <node/miner.h>
#include <streams.h>
#include <uint256.h>
#include <util/sock.h>

/**
 * A type mainly used as the message length field in stratum v2 messages.
 */
using u24_t = uint8_t[3];

/**
 * The template provider subprotocol used in setup connection messages. The stratum v2
 * template provider only recognizes its own subprotocol.
 */
static constexpr uint8_t SETUP_CONN_TP_PROTOCOL{0x02};

/**
 * All the stratum v2 message types handled by the template provider.
 */
enum Sv2MsgType : uint8_t {
    SETUP_CONNECTION = 0x00,
    SETUP_CONNECTION_SUCCESS = 0x01,
    NEW_TEMPLATE = 0x71,
    SET_NEW_PREV_HASH = 0x72,
    SUBMIT_SOLUTION = 0x76,
    COINBASE_OUTPUT_DATA_SIZE = 0x70,
};

/**
 * Base class for all stratum v2 messages.
 */
class Sv2Msg
{
public:
    // Returns the message type for a Stratum V2 message.
    virtual Sv2MsgType GetMsgType() const = 0;

    void ReadSTR0_255(CDataStream& stream, std::string& output) const
    {
        uint8_t len;
        stream >> len;

        for (auto i = 0; i < len; ++i) {
            uint8_t b;
            stream >> b;

            output.push_back(b);
        }
    }
};

/**
 * The first message sent by the client to the server to establish a connection
 * and specifies the subprotocol (Template Provider).
 */
class SetupConnectionMsg : Sv2Msg
{
private:
    /**
     * The default message type value for this Stratum V2 message.
     */
    static const auto m_msg_type = Sv2MsgType::SETUP_CONNECTION;

public:
    /**
     * Specifies the subprotocol for the new connection. It will always be TemplateDistribution
     * (0x02).
     */
    uint8_t m_protocol;

    /**
     * The minimum protocol version the client supports (currently must be 2).
     */
    uint16_t m_min_version;

    /**
     * The maximum protocol version the client supports (currently must be 2).
     */
    uint16_t m_max_version;

    /**
     * Flags indicating optional protocol features the client supports. Each protocol
     * from protocol field has its own values/flags.
     */
    uint32_t m_flags;

    /**
     * ASCII text indicating the hostname or IP address.
     */
    std::string m_endpoint_host;

    /**
     * Connecting port value.
     */
    uint16_t m_endpoint_port;

    /**
     * Vendor name of the connecting device.
     */
    std::string m_vendor;

    /**
     * Hardware version of the connecting device.
     */
    std::string m_hardware_version;

    /**
     * Firmware of the connecting device.
     */
    std::string m_firmware;

    /**
     * Unique identifier of the device as defined by the vendor.
     */
    std::string m_device_id;

    /**
     * Sv2Msg Implementation.
     */
    Sv2MsgType GetMsgType() const override
    {
        return m_msg_type;
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> m_protocol >> m_min_version >> m_max_version >> m_flags;

        ReadSTR0_255(s, m_endpoint_host);
        s >> m_endpoint_port;
        ReadSTR0_255(s, m_vendor);
        ReadSTR0_255(s, m_hardware_version);
        ReadSTR0_255(s, m_firmware);
        ReadSTR0_255(s, m_device_id);
    }
};

/**
 * Set the coinbase outputs data len for the outputs that the client want to add to the coinbase.
 * The TP MUST NOT provide NewWork messages which would represent consensus-invalid blocks once this
 * additional size — along with a maximally-sized (100 byte) coinbase field — is added.
 */
class CoinbaseOutputDataSizeMsg : Sv2Msg
{
private:
    /**
     * The default message type value for this Stratum V2 message.
     */
    static const auto m_msg_type = Sv2MsgType::COINBASE_OUTPUT_DATA_SIZE;

public:
    /**
     * The maximum additional serialized bytes which the pool will add in coinbase transaction outputs
     */
    uint32_t m_coinbase_output_max_additional_size;


    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> m_coinbase_output_max_additional_size;
    }

    /**
     * Sv2Msg Implementation.
     */
    Sv2MsgType GetMsgType() const override
    {
        return m_msg_type;
    }
};

/**
 * Response to the SetupConnection message if the server accepts the connection.
 * The client is required to verify the set of feature flags that the server
 * supports and act accordingly.
 */
class SetupConnectionSuccessMsg : Sv2Msg
{
private:
    /**
     * The default message type value for this Stratum V2 message.
     */
    static const auto m_msg_type = Sv2MsgType::SETUP_CONNECTION_SUCCESS;

public:
    /**
     * Selected version proposed by the connecting node that the upstream node supports.
     * This version will be used on the connection for the rest of its life.
     */
    uint16_t m_used_version;

    /**
     * Flags indicating optional protocol features the server supports. Each protocol
     * from protocol field has its own values/flags.
     */
    uint32_t m_flags;

    explicit SetupConnectionSuccessMsg(uint16_t used_version, uint32_t flags) : m_used_version{used_version}, m_flags{flags} {};

    /**
     * Sv2Msg Implementation.
     */
    Sv2MsgType GetMsgType() const override
    {
        return m_msg_type;
    }

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << m_used_version
          << m_flags;
    }
};

/**
 * The work template for downstream devices. Can be used for future work or immediate work.
 * The NewTemplate will be matched to a cached block using the template id.
 */
class NewTemplateMsg : Sv2Msg
{
private:
    /**
     * The default message type value for this Stratum V2 message.
     */
    static const auto m_msg_type = Sv2MsgType::NEW_TEMPLATE;

public:
    /**
     * Server’s identification of the template. Strictly increasing, the current UNIX
     * time may be used in place of an ID.
     */
    uint64_t m_template_id;

    /**
     * True if the template is intended for future SetNewPrevHash message sent on the channel.
     * If False, the job relates to the last sent SetNewPrevHash message on the channel
     * and the miner should start to work on the job immediately. */
    bool m_future_template;

    /**
     * Valid header version field that reflects the current network consensus.
     * The general purpose bits (as specified in BIP320) can be freely manipulated
     * by the downstream node. The downstream node MUST NOT rely on the upstream
     * node to set the BIP320 bits to any particular value.
     */
    uint32_t m_version;

    /**
     * The coinbase transaction nVersion field.
     */
    uint32_t m_coinbase_tx_version;

    /**
     * Up to 8 bytes (not including the length byte) which are to be placed at
     * the beginning of the coinbase field in the coinbase transaction.
     */
    CScript m_coinbase_prefix;

    /**
     * The coinbase transaction input’s nSequence field.
     */
    uint32_t m_coinbase_tx_input_sequence;

    /**
     * The value, in satoshis, available for spending in coinbase outputs added
     * by the client. Includes both transaction fees and block subsidy.
     */
    uint64_t m_coinbase_tx_value_remaining;

    /**
     * The number of transaction outputs included in coinbase_tx_outputs.
     */
    uint32_t m_coinbase_tx_outputs_count;

    /**
     * Bitcoin transaction outputs to be included as the last outputs in the coinbase transaction.
     */
    std::vector<CTxOut> m_coinbase_tx_outputs;

    /**
     * The locktime field in the coinbase transaction.
     */
    uint32_t m_coinbase_tx_locktime;

    /**
     * Merkle path hashes ordered from deepest.
     */
    std::vector<uint256> m_merkle_path;

    /**
     * Sv2Msg Implementation.
     */
    Sv2MsgType GetMsgType() const override
    {
        return m_msg_type;
    }

    NewTemplateMsg() = default;

    explicit NewTemplateMsg(const CBlock& block, uint64_t template_id, bool future_template)
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

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << m_template_id
          << m_future_template
          << m_version
          << m_coinbase_tx_version
          << m_coinbase_prefix
          << m_coinbase_tx_input_sequence
          << m_coinbase_tx_value_remaining;
        if (m_coinbase_tx_outputs_count > 0) {
            s << static_cast<uint32_t>(m_coinbase_tx_outputs_count);

            std::vector<uint8_t> outputs_bytes;
            CVectorWriter{SER_NETWORK, PROTOCOL_VERSION, outputs_bytes, 0, m_coinbase_tx_outputs[0]};

            s << static_cast<uint16_t>(outputs_bytes.size());
            s.write(MakeByteSpan(outputs_bytes));
        } else {
            // We still need to send 2 bytes indicating an empty coinbase-tx_outputs array. */
            s << static_cast<uint16_t>(0);
        }
        s << m_coinbase_tx_locktime
          << m_merkle_path;
    }
};

/**
 * When the template provider creates a new valid best block, the template provider
 * MUST immediately send the SetNewPrevHash  message. This message can also be used
 * for a future template, indicating the client can begin work on a previously
 * received and cached NewTemplate which contains the same template id.
 */
class SetNewPrevHashMsg : Sv2Msg
{
private:
    /**
     * The default message type value for this Stratum V2 message.
     */
    static const auto m_msg_type = Sv2MsgType::SET_NEW_PREV_HASH;

public:
    /**
     * The id referenced in a previous NewTemplate message.
     */
    uint64_t m_template_id;

    /**
     * Previous block’s hash, as it must appear in the next block’s header.
     */
    uint256 m_prev_hash;

    /**
     * The nTime field in the block header at which the client should start (usually current time).
     * This is NOT the minimum valid nTime value.
     */
    uint32_t m_header_timestamp;

    /**
     * Block header field.
     */
    uint32_t m_nBits;

    /**
     * The maximum double-SHA256 hash value which would represent a valid block.
     * Note that this may be lower than the target implied by nBits in several cases,
     * including weak-block based block propagation.
     */
    uint256 m_target;

    SetNewPrevHashMsg() = default;

    /**
     * Sv2Msg Implementation.
     */
    Sv2MsgType GetMsgType() const override
    {
        return m_msg_type;
    }

    explicit SetNewPrevHashMsg(const CBlock& block, uint64_t template_id)
    {
        m_template_id = template_id;
        m_prev_hash = block.hashPrevBlock;
        m_header_timestamp = block.nTime;
        m_nBits = block.nBits;
        m_target = ArithToUint256(arith_uint256().SetCompact(block.nBits));
    }

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << m_template_id
          << m_prev_hash
          << m_header_timestamp
          << m_nBits
          << m_target;
    }
};

/**
 * The client sends a SubmitSolution after finding a coinbase transaction/nonce
 * pair which double-SHA256 hashes at or below SetNewPrevHash::target. The template provider
 * finds the cached block according to the template id and reconstructs the block with the
 * values from SubmitSolution. The template provider must then propagate the block to the
 * Bitcoin Network.
 */
class SubmitSolutionMsg : Sv2Msg
{
private:
    /**
     * The default message type value for this Stratum V2 message.
     */
    static const auto m_msg_type = Sv2MsgType::SUBMIT_SOLUTION;

public:
    /**
     * The id referenced in a NewTemplate.
     */
    uint64_t m_template_id;

    /**
     * The version field in the block header. Bits not defined by BIP320 as additional
     * nonce MUST be the same as they appear in the NewWork message, other bits may
     * be set to any value.
     */
    uint32_t m_version;

    /**
     * The nTime field in the block header. This MUST be greater than or equal to
     * the header_timestamp field in the latest SetNewPrevHash message and lower
     * than or equal to that value plus the number of seconds since the receipt
     * of that message.
     */
    uint32_t m_header_timestamp;

    /**
     * The nonce field in the header.
     */
    uint32_t m_header_nonce;

    /**
     * The full serialized coinbase transaction, meeting all the requirements of the NewWork message, above.
     */
    CMutableTransaction m_coinbase_tx;

    SubmitSolutionMsg() = default;

    /**
     * Sv2Msg Implementation.
     */
    Sv2MsgType GetMsgType() const override
    {
        return m_msg_type;
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> m_template_id >> m_version >> m_header_timestamp >> m_header_nonce;

        // Ignore the 2 byte length as the rest of the stream is assumed to be
        // the m_coinbase_tx.
        s.ignore(2);
        s >> m_coinbase_tx;
    }
};

/**
 * Header for all stratum v2 messages. Each header must contain the message type,
 * the length of the serialized message and a 2 byte extension field currently
 * not utilised by the template provider.
 */
class Sv2NetHeader
{
public:
    /**
     * Unique identifier of the message.
     */
    Sv2MsgType m_msg_type;

    /**
     * Serialized length of the message.
     */
    uint32_t m_msg_len;

    Sv2NetHeader() = default;
    explicit Sv2NetHeader(Sv2MsgType msg_type, uint32_t msg_len) : m_msg_type{msg_type}, m_msg_len{msg_len} {};

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        // The Template Provider currently does not use the extension_type field,
        // but the field is still required for all headers.
        uint16_t extension_type = 0;

        u24_t msg_len;
        msg_len[2] = (m_msg_len >> 16) & 0xff;
        msg_len[1] = (m_msg_len >> 8) & 0xff;
        msg_len[0] = m_msg_len & 0xff;

        s << extension_type
          << static_cast<uint8_t>(m_msg_type)
          << msg_len;
    };

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        // Ignore the extension type (2 bytes) as the Template Provider currently doesn't
        // interpret this field.
        s.ignore(2);

        uint8_t msg_type;
        s >> msg_type;
        m_msg_type = static_cast<Sv2MsgType>(msg_type);

        u24_t msg_len_bytes;
        for (unsigned int i = 0; i < sizeof(u24_t); ++i) {
            s >> msg_len_bytes[i];
        }

        m_msg_len = msg_len_bytes[2];
        m_msg_len = m_msg_len << 8 | msg_len_bytes[1];
        m_msg_len = m_msg_len << 8 | msg_len_bytes[0];
    }
};

/**
 * The networked form for all stratum v2 messages, contains a header and a serialized
 * payload from a referenced stratum v2 message.
 */
template <typename M>
class Sv2NetMsg
{
private:
    Sv2NetHeader m_sv2_header;
    std::vector<uint8_t> m_msg;

public:
    explicit Sv2NetMsg(const M& msg)
    {
        CVectorWriter{SER_NETWORK, PROTOCOL_VERSION, m_msg, 0, msg};
        m_sv2_header = Sv2NetHeader{msg.GetMsgType(), static_cast<uint32_t>(m_msg.size())};
    }

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << m_sv2_header;
        s.write(MakeByteSpan(m_msg));
    }
};

class Sv2Client
{
public:
    /**
     * Receiving and sending socket for the connected client
     */
    std::shared_ptr<Sock> m_sock;

    /**
     * Whether the client has confirmed the connection with a successful SetupConnection.
     */
    bool m_setup_connection_confirmed;

    /**
     * Whether the client is a candidate for disconnection.
     */
    bool m_disconnect_flag;

    /**
     * Whether the client has been received CoinbaseOutputDataSize message.
     */
    bool m_coinbase_output_data_size_recv;

    // TODO: Add comment.
    unsigned int m_coinbase_tx_outputs_size;

    explicit Sv2Client(std::shared_ptr<Sock> sock) : m_sock{sock} {};
};

/**
 * The main class that runs the template provider server.
 */
class Sv2TemplateProvider
{
private:
    /**
     * The main listening socket for new stratum v2 connections.
     */
    std::shared_ptr<Sock> m_listening_socket;

    /**
     * The main thread for the template provider.
     */
    std::thread m_thread_sv2_handler;

    /**
     * A list of all connected stratum v2 clients.
     */
    std::vector<Sv2Client> m_sv2_clients;

    /**
     * Signal for handling interrupts and stopping the template provider event loop.
     */
    std::atomic<bool> m_flag_interrupt_sv2{false};
    CThreadInterrupt m_interrupt_sv2;

    /**
     * ChainstateManager and CTxMemPool are both used to build new valid blocks,
     * getting the best known block hash and checking whether the node is still
     * in IBD.
     */
    ChainstateManager& m_chainman;
    CTxMemPool& m_mempool;

    /**
     * A cache that maps ids used in NewTemplate messages and its associated block.
     */
    std::map<uint64_t, std::unique_ptr<node::CBlockTemplate>> m_blocks_cache;

    /**
     * The best known template to give all sv2 clients.
     */
    NewTemplateMsg m_new_template;

    /**
     * The current best known new template id. It is incremented on each new template.
     */
    uint64_t m_template_id;

    /**
     * The current best known SetNewPrevHash that references the current best known
     * block hash as the previous hash.
     */
    SetNewPrevHashMsg m_best_prev_hash;

    /**
     * Creates a socket and binds the port for new stratum v2 connections.
     */
    void BindListenPort(uint16_t port);

    /**
     * The main thread for the template provider, contains an event loop handling
     * all tasks for the template provider.
     */
    void ThreadSv2Handler();

    /**
     * Builds a new block, caches it and builds the most recent and best NewTemplate from the new block.
     */
    void UpdateTemplate(bool future, unsigned int coinbase_tx_outputs_size);

    /**
     * Builds a new SetNewPrevHash referencing the best NewTemplate.
     */
    void UpdatePrevHash();

    /**
     * Called when a best new block is found on the network. The template provider will notify and
     * send the best NewTemplate and SetNewPrevHash to all clients.
     */
    void OnNewBlock();

    /**
     * Main handler for all received stratum v2 messages.
     */
    void ProcessSv2Message(const Sv2NetHeader& sv2_header, CDataStream& ss, Sv2Client& client);

    /**
     * Generates the socket events for each Sv2Client socket and the main listening socket.
     */
    Sock::EventsPerSock GenerateWaitSockets() const;
public:
    explicit Sv2TemplateProvider(ChainstateManager& chainman, CTxMemPool& mempool) : m_chainman{chainman}, m_mempool{mempool} {};
    /**
     * Starts the template provider server and thread.
     */
    void Start(uint16_t port);

    /**
     * Tear down of the template provider thread and any other necessary tear down.
     */
    void StopThreads();

    /**
     * Triggered on interrupt signals to stop the main event loop in ThreadSv2Handler().
     */
    void Interrupt();
};

#endif // BITCOIN_SV2_TEMPLATE_PROVIDER_H
