// TODO: Remove unused imports
#include <chainparams.h>
#include <streams.h>
#include <uint256.h>
#include <miner.h>

#include <sv2_distributor.h>
#include <time.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(sv2_block_template_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(create_sv2_block_template)
{
    // TODO: Sections - Add txtransctions to the mempool
    TestMemPoolEntryHelper entry;

    CKey parent_key;
    parent_key.MakeNewKey(true);
    CScript parent_locking_script = GetScriptForDestination(PKHash(parent_key.GetPubKey()));
    auto parent = CreateValidMempoolTransaction(/* input_transaction */ m_coinbase_txns[0], /* vout */ 0,
                                                    /* input_height */ 0, /* input_signing_key */ coinbaseKey,
                                                    /* output_destination */ parent_locking_script,
                                                    /* output_amount */ CAmount(49 * COIN), /* submit */ true);
    CTransactionRef tx_parent = MakeTransactionRef(parent);


    // TODO: Make multiple transactions to add to the mempool.
    CKey child_key;
    child_key.MakeNewKey(true);
    CScript child_locking_script = GetScriptForDestination(PKHash(child_key.GetPubKey()));
    auto child = CreateValidMempoolTransaction(/* input_transaction */ tx_parent, /* vout */ 0,
                                                   /* input_height */ 101, /* input_signing_key */ parent_key,
                                                   /* output_destination */ child_locking_script,
                                                   /* output_amount */ CAmount(48 * COIN), /* submit */ true);


    // TODO: Sections - Creating Block via the BlockAssembler
    std::unique_ptr<const CChainParams> chainParams = CreateChainParams(*m_node.args, CBaseChainParams::MAIN);
    const CChainParams& chainparams = *chainParams;

    BlockAssembler::Options options;
    options.nBlockMaxWeight = MAX_BLOCK_WEIGHT;
    options.blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);

    CNewTemplate ctemplate = AssembleSv2BlockTemplate(m_node.chainman->ActiveChainstate(), *m_node.mempool, chainparams, options);

    // TODO: Sections - Build the message on the wire using the CNewTemplate message.
    CSv2Message message;
    message.tag = CSv2Message::Tag::NewTemplate;
    message.new_template._0 = ctemplate;

    // TODO: Sections - Encoding section.
    EncoderWrapper* encoder = new_encoder();
    CResult<CVec, Sv2Error> encoded = encode(&message, encoder);

    switch (encoded.tag) {
    case CResult < CVec, Sv2Error > ::Tag::Ok:
        std::cout << "Encoding was OK" << std::endl;
        break;
    case CResult < CVec, Sv2Error > ::Tag::Err:
        std::cout << "ERROR OCCURRED: " << encoded.err._0 << std::endl;
        // TODO: Assert the failure of encoding here.
        return;
        /* break; */
    }

    // TODO: Sections - Decoding section.
    DecoderWrapper* decoder = new_decoder();

    int byte_read = 0;
    bool decoded = false;
    CNewTemplate decoded_msg;
    while (!decoded) {
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

        CResult <CSv2Message, Sv2Error> frame = next_frame(decoder);

        switch (frame.tag) {
          case CResult < CSv2Message, Sv2Error > ::Tag::Ok:
            decoded = true;
            decoded_msg = frame.ok._0.new_template._0;
            break;
          case CResult < CSv2Message, Sv2Error > ::Tag::Err:
            break;
            // TODO: Assert the failure of decoding here
          };
    }

    // TODO: Sections - Comparison section
    BOOST_REQUIRE_EQUAL(decoded_msg.template_id, ctemplate.template_id);
    BOOST_REQUIRE_EQUAL(decoded_msg.future_template, ctemplate.future_template);
    BOOST_REQUIRE_EQUAL(decoded_msg.version, ctemplate.version);
    BOOST_REQUIRE_EQUAL(decoded_msg.coinbase_tx_version, ctemplate.coinbase_tx_version);
    BOOST_REQUIRE_EQUAL(memcmp(decoded_msg.coinbase_prefix.data, ctemplate.coinbase_prefix.data, decoded_msg.coinbase_prefix.len), 0);
    BOOST_REQUIRE_EQUAL(decoded_msg.coinbase_prefix.len, ctemplate.coinbase_prefix.len);
    BOOST_REQUIRE_EQUAL(memcmp(decoded_msg.coinbase_tx_outputs.data, ctemplate.coinbase_tx_outputs.data, decoded_msg.coinbase_tx_outputs.len), 0);

    std::cout << "DEBUG: merkle_path len: " << ctemplate.merkle_path.len << std::endl;
    std::cout << "DEBUG: decoded merkle_path len: " << decoded_msg.merkle_path.len << std::endl;
    BOOST_REQUIRE_EQUAL(decoded_msg.merkle_path.len, ctemplate.merkle_path.len);
    BOOST_REQUIRE_EQUAL(decoded_msg.merkle_path.capacity, ctemplate.merkle_path.capacity);
    BOOST_REQUIRE_EQUAL(memcmp(decoded_msg.coinbase_tx_outputs.data, ctemplate.coinbase_tx_outputs.data, decoded_msg.coinbase_tx_outputs.len), 0);

    drop_sv2_message(message);
}

BOOST_AUTO_TEST_SUITE_END()
