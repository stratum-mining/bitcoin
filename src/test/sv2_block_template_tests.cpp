#include <chainparams.h>
#include <streams.h>
#include <uint256.h>
#include <miner.h>

#include <sv2_distributor.h>
#include <time.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>


// CCDLE12: TMP - Remove once mock mempool can be used.
namespace sv2_block_template_tests {
struct Sv2BlockTemplateTestingSetup : public TestingSetup {

    void TestPackageSelection(
		    const CChainParams& chainparams, 
		    const CScript& scriptPubKey, 
		    const std::vector<CTransactionRef>& txFirst) 
	    EXCLUSIVE_LOCKS_REQUIRED(::cs_main, m_node.mempool->cs);

    bool TestSequenceLocks(const CTransaction& tx, int flags) 
	    EXCLUSIVE_LOCKS_REQUIRED(::cs_main, m_node.mempool->cs)
    {
        CCoinsViewMemPool view_mempool(&m_node.chainman->ActiveChainstate().CoinsTip(), *m_node.mempool);
        return CheckSequenceLocks(m_node.chainman->ActiveChain().Tip(), view_mempool, tx, flags);
    }
};
} // namespace sv2_block_template_tests

BOOST_FIXTURE_TEST_SUITE(sv2_block_template_tests, Sv2BlockTemplateTestingSetup)

BOOST_AUTO_TEST_CASE(create_sv2_block_template)
{
    const auto chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const CChainParams& chainparams = *chainParams;

    BlockAssembler::Options options;
    options.nBlockMaxWeight = MAX_BLOCK_WEIGHT;
    options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);

    CNewTemplate ctemplate = AssembleSv2BlockTemplate(m_node.chainman->ActiveChainstate(), *m_node.mempool, chainparams, options);

    CSv2Message message;
    message.tag = CSv2Message::Tag::NewTemplate;
    message.new_template._0 = ctemplate;

    EncoderWrapper* encoder = new_encoder();
    CResult<CVec, Sv2Error> encoded = encode(&message, encoder);

    // CCDLE12: TMP Debug
    switch (encoded.tag) {
    case CResult < CVec, Sv2Error > ::Tag::Ok:
        std::cout << "Encoding was OK" << std::endl;
        break;
    case CResult < CVec, Sv2Error > ::Tag::Err:
        std::cout << "ERROR OCCURRED: " << encoded.err._0 << std::endl;
        /* break; */
        return;
    }

    DecoderWrapper* decoder = new_decoder();

    int byte_read = 0;
    bool decoded = false;
    CNewTemplate decoded_msg;
    while (! decoded) {
        // This is thought to works with streams of data from which you read new bytes when they
        // arrive so more something like that:
        // ```
        // CVec buffer = get_writable(decoder);
        // while (byte_read < buffer.len) {
        //   byte_read += read(new_socket, buffer.data, (buffer.len - byte_read));

        // }
        // byte_read = 0;
        // ```

        CVec buffer = get_writable(decoder);
        memcpy(buffer.data, &encoded.ok._0.data[byte_read], buffer.len);
        byte_read += buffer.len;

        CResult < CSv2Message, Sv2Error > frame = next_frame(decoder);

        switch (frame.tag) {
          case CResult < CSv2Message, Sv2Error > ::Tag::Ok:
            std::cout << "\n";
            std::cout << "OK";
            std::cout << "\n";
            decoded = true;
            decoded_msg = frame.ok._0.new_template._0;
            break;
          case CResult < CSv2Message, Sv2Error > ::Tag::Err:
            break;
          };
    }

    BOOST_CHECK(decoded_msg.template_id == ctemplate.template_id);
    BOOST_CHECK(decoded_msg.future_template == ctemplate.future_template);
    BOOST_CHECK(decoded_msg.version == ctemplate.version);
    BOOST_CHECK(decoded_msg.coinbase_tx_version == ctemplate.coinbase_tx_version);
    BOOST_CHECK(memcmp(decoded_msg.coinbase_prefix.data, ctemplate.coinbase_prefix.data, decoded_msg.coinbase_prefix.len) == 0);
    BOOST_CHECK(decoded_msg.coinbase_prefix.len == ctemplate.coinbase_prefix.len);
    BOOST_CHECK(memcmp(decoded_msg.coinbase_tx_outputs.data, ctemplate.coinbase_tx_outputs.data, decoded_msg.coinbase_tx_outputs.len) == 0);

    std::cout << "DEBUG: merkle_path len: " << ctemplate.merkle_path.len << std::endl;
    std::cout << "DEBUG: decoded merkle_path len: " << decoded_msg.merkle_path.len << std::endl;
    BOOST_CHECK(decoded_msg.merkle_path.len == ctemplate.merkle_path.len);
    BOOST_CHECK(decoded_msg.merkle_path.capacity == ctemplate.merkle_path.capacity);
    BOOST_CHECK(memcmp(decoded_msg.coinbase_tx_outputs.data, ctemplate.coinbase_tx_outputs.data, decoded_msg.coinbase_tx_outputs.len) == 0);


    /* free_vec_2(); */
    drop_sv2_message(message);
}

BOOST_AUTO_TEST_SUITE_END()
