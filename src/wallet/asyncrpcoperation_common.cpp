#include "asyncrpcoperation_common.h"

#include "core_io.h"
#include "init.h"
#include "rpc/protocol.h"
#include "util/moneystr.h"

extern UniValue signrawtransaction(const UniValue& params, bool fHelp);

UniValue SendEffectedTransaction(
        const CTransaction& tx,
        const TransactionEffects& effects,
        std::optional<std::reference_wrapper<CReserveKey>> reservekey,
        bool testmode)
{
    UniValue o(UniValue::VOBJ);
    // Send the transaction
    if (!testmode) {
        CWalletTx wtx(pwalletMain, tx);
        // save the mapping from (receiver, txid) to UA
        if (!pwalletMain->SaveRecipientMappings(tx.GetHash(), effects.GetPayments().GetResolvedPayments())) {
            effects.UnlockSpendable();
            // More details in debug log
            throw JSONRPCError(RPC_WALLET_ERROR, "SendTransaction: SaveRecipientMappings failed");
        }
        CValidationState state;
        if (!pwalletMain->CommitTransaction(wtx, reservekey, state)) {
            effects.UnlockSpendable();
            std::string strError = strprintf("SendTransaction: Transaction commit failed:: %s", state.GetRejectReason());
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }
        o.pushKV("txid", tx.GetHash().ToString());
    } else {
        // Test mode does not send the transaction to the network nor save the recipient mappings.
        o.pushKV("test", 1);
        o.pushKV("txid", tx.GetHash().ToString());
        o.pushKV("hex", EncodeHexTx(tx));
    }
    effects.UnlockSpendable();
    return o;
}

std::pair<CTransaction, UniValue> SignSendRawTransaction(UniValue obj, std::optional<std::reference_wrapper<CReserveKey>> reservekey, bool testmode) {
    // Sign the raw transaction
    UniValue rawtxnValue = find_value(obj, "rawtxn");
    if (rawtxnValue.isNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Missing hex data for raw transaction");
    }
    std::string rawtxn = rawtxnValue.get_str();

    UniValue params = UniValue(UniValue::VARR);
    params.push_back(rawtxn);
    UniValue signResultValue = signrawtransaction(params, false);
    UniValue signResultObject = signResultValue.get_obj();
    UniValue completeValue = find_value(signResultObject, "complete");
    bool complete = completeValue.get_bool();
    if (!complete) {
        // TODO: #1366 Maybe get "errors" and print array vErrors into a string
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Failed to sign transaction");
    }

    UniValue hexValue = find_value(signResultObject, "hex");
    if (hexValue.isNull()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Missing hex data for signed transaction");
    }
    std::string signedtxn = hexValue.get_str();
    CDataStream stream(ParseHex(signedtxn), SER_NETWORK, PROTOCOL_VERSION);
    CTransaction tx;
    stream >> tx;

    // Recipient mappings are not available when sending a raw transaction.
    std::vector<RecipientMapping> recipientMappings;
    UniValue sendResult = SendTransaction(tx, recipientMappings, reservekey, testmode);

    return std::make_pair(tx, sendResult);
}

void ThrowInputSelectionError(
        const InputSelectionError& err,
        const ZTXOSelector& selector,
        const TransactionStrategy& strategy)
{
    std::visit(match {
        [](const AddressResolutionError& err) {
            switch (err) {
                case AddressResolutionError::SproutSpendNotPermitted:
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "Sending from the Sprout shielded pool to the Sapling "
                        "shielded pool is not enabled by default because it will "
                        "publicly reveal the transaction amount. THIS MAY AFFECT YOUR PRIVACY. "
                        "Resubmit with the `privacyPolicy` parameter set to `AllowRevealedAmounts` "
                        "or weaker if you wish to allow this transaction to proceed anyway.");
                case AddressResolutionError::SproutRecipientNotPermitted:
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "Sending funds into the Sprout pool is no longer supported.");
                case AddressResolutionError::TransparentRecipientNotPermitted:
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "This transaction would have transparent recipients, which is not "
                        "enabled by default because it will publicly reveal transaction "
                        "recipients and amounts. THIS MAY AFFECT YOUR PRIVACY. Resubmit "
                        "with the `privacyPolicy` parameter set to `AllowRevealedRecipients` "
                        "or weaker if you wish to allow this transaction to proceed anyway.");
                case AddressResolutionError::InsufficientSaplingFunds:
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "Sending from the Sapling shielded pool to the Orchard "
                        "shielded pool is not enabled by default because it will "
                        "publicly reveal the transaction amount. THIS MAY AFFECT YOUR PRIVACY. "
                        "Resubmit with the `privacyPolicy` parameter set to `AllowRevealedAmounts` "
                        "or weaker if you wish to allow this transaction to proceed anyway.");
                case AddressResolutionError::UnifiedAddressResolutionError:
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "Could not select a unified address receiver that allows this transaction "
                        "to proceed without publicly revealing the transaction amount. THIS MAY AFFECT "
                        "YOUR PRIVACY. Resubmit with the `privacyPolicy` parameter set to "
                        "`AllowRevealedAmounts` or weaker if you wish to allow this transaction to "
                        "proceed anyway.");
                case AddressResolutionError::ChangeAddressSelectionError:
                    // this should be unreachable, but we handle it defensively
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "Could not select a change address that allows this transaction "
                        "to proceed without publicly revealing transaction details. THIS MAY AFFECT "
                        "YOUR PRIVACY. Resubmit with the `privacyPolicy` parameter set to "
                        "`AllowRevealedAmounts` or weaker if you wish to allow this transaction to "
                        "proceed anyway.");
                default:
                    assert(false);
            }
        },
        [&](const InvalidFundsError& err) {
            bool isFromUa = std::holds_alternative<libzcash::UnifiedAddress>(selector.GetPattern());
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                strprintf(
                    "Insufficient funds: have %s, %s",
                    FormatMoney(err.available),
                    std::visit(match {
                        [](const InsufficientFundsError& ife) {
                            return strprintf("need %s", FormatMoney(ife.required));
                        },
                        [](const DustThresholdError& dte) {
                            return strprintf(
                                    "need %s more to avoid creating invalid change output %s (dust threshold is %s)",
                                    FormatMoney(dte.dustThreshold - dte.changeAmount),
                                    FormatMoney(dte.changeAmount),
                                    FormatMoney(dte.dustThreshold));
                        }
                    },
                    err.reason))
                    + (err.transparentCoinbasePermitted
                       ? "" :
                       "; note that coinbase outputs will not be selected if any transparent "
                       "recipients are included or if the `privacyPolicy` parameter is not set to "
                       "`AllowRevealedSenders` or weaker")
                    + (selector.SelectsTransparentCoinbase()
                       ? "" :
                       "; note that coinbase outputs will not be selected if you specify "
                       "ANY_TADDR")
                    + (!isFromUa || strategy.AllowLinkingAccountAddresses() ? "." :
                       ". (This transaction may require selecting transparent coins that were sent "
                       "to multiple Unified Addresses, which is not enabled by default because "
                       "it would create a public link between the transparent receivers of these "
                       "addresses. THIS MAY AFFECT YOUR PRIVACY. Resubmit with the `privacyPolicy` "
                       "parameter set to `AllowLinkingAccountAddresses` or weaker if you wish to "
                       "allow this transaction to proceed anyway.)"));
        },
        [](const ChangeNotAllowedError& err) {
            throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    strprintf(
                        "When shielding coinbase funds, the wallet does not allow any change. "
                        "The proposed transaction would result in %s in change.",
                        FormatMoney(err.available - err.required)));
        },
        [](const ExcessOrchardActionsError& err) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                strprintf(
                    "Attempting to spend %u Orchard notes would exceed the current limit "
                    "of %u notes, which exists to prevent memory exhaustion. Restart with "
                    "`-orchardactionlimit=N` where N >= %u to allow the wallet to attempt "
                    "to construct this transaction.",
                    err.orchardNotes,
                    err.maxNotes,
                    err.orchardNotes));
        }
    }, err);
}
