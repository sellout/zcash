// Copyright (c) 2022 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef ZCASH_WALLET_WALLET_TX_BUILDER_H
#define ZCASH_WALLET_WALLET_TX_BUILDER_H

#include "consensus/params.h"
#include "transaction_builder.h"
#include "wallet/memo.h"
#include "wallet/wallet.h"

using namespace libzcash;

int GetAnchorHeight(const CChain& chain, int anchorConfirmations);

/**
 * A payment that has been resolved to send to a specific
 * recipient address in a single pool.
 */
class ResolvedPayment : public RecipientMapping {
public:
    CAmount amount;
    std::optional<Memo> memo;
    bool isInternal{false};

    ResolvedPayment(
            std::optional<libzcash::UnifiedAddress> ua,
            libzcash::RecipientAddress address,
            CAmount amount,
            std::optional<Memo> memo,
            bool isInternal) :
        RecipientMapping(ua, address), amount(amount), memo(memo), isInternal(isInternal) {}
};

/**
 * A requested payment that has not yet been resolved to a
 * specific recipient address.
 */
class Payment {
private:
    PaymentAddress address;
    CAmount amount;
    std::optional<Memo> memo;
    bool isInternal;
public:
    Payment(
            PaymentAddress address,
            CAmount amount,
            std::optional<Memo> memo,
            bool isInternal = false) :
        address(address), amount(amount), memo(memo), isInternal(isInternal) {}

    const PaymentAddress& GetAddress() const {
        return address;
    }

    CAmount GetAmount() const {
        return amount;
    }

    const std::optional<Memo>& GetMemo() const {
        return memo;
    }

    bool IsInternal() const {
        return isInternal;
    }
};

class Payments {
private:
    std::vector<ResolvedPayment> payments;
    std::set<OutputPool> recipientPools;
    CAmount t_outputs_total{0};
    CAmount sapling_outputs_total{0};
    CAmount orchard_outputs_total{0};
public:
    Payments(std::vector<ResolvedPayment> payments) {
        for (const ResolvedPayment& payment : payments) {
            AddPayment(payment);
        }
    }

    void AddPayment(ResolvedPayment payment) {
        std::visit(match {
            [&](const CKeyID& addr) {
                t_outputs_total += payment.amount;
                recipientPools.insert(OutputPool::Transparent);
            },
            [&](const CScriptID& addr) {
                t_outputs_total += payment.amount;
                recipientPools.insert(OutputPool::Transparent);
            },
            [&](const libzcash::SaplingPaymentAddress& addr) {
                sapling_outputs_total += payment.amount;
                recipientPools.insert(OutputPool::Sapling);
            },
            [&](const libzcash::OrchardRawAddress& addr) {
                orchard_outputs_total += payment.amount;
                recipientPools.insert(OutputPool::Orchard);
            }
        }, payment.address);
        payments.push_back(payment);
    }

    const std::set<OutputPool>& GetRecipientPools() const {
        return recipientPools;
    }

    bool HasTransparentRecipient() const {
        return recipientPools.count(OutputPool::Transparent) > 0;
    }

    bool HasSaplingRecipient() const {
        return recipientPools.count(OutputPool::Sapling) > 0;
    }

    bool HasOrchardRecipient() const {
        return recipientPools.count(OutputPool::Orchard) > 0;
    }

    const std::vector<ResolvedPayment>& GetResolvedPayments() const {
        return payments;
    }

    CAmount GetTransparentBalance() const {
        return t_outputs_total;
    }

    CAmount GetSaplingBalance() const {
        return sapling_outputs_total;
    }

    CAmount GetOrchardBalance() const {
        return orchard_outputs_total;
    }

    CAmount Total() const {
        return
            t_outputs_total +
            sapling_outputs_total +
            orchard_outputs_total;
    }
};

typedef std::variant<
    RecipientAddress,
    SproutPaymentAddress> ChangeAddress;

class TransactionEffects {
private:
    AccountId sendFromAccount;
    uint32_t anchorConfirmations{1};
    SpendableInputs spendable;
    Payments payments;
    std::optional<ChangeAddress> changeAddr;
    CAmount fee{0};
    uint256 internalOVK;
    uint256 externalOVK;
    // TODO: This needs to be richer, like an `anchorBlock`, so the `TransactionEffects` can
    //       be recalculated if the state of the chain has changed significantly between the time of
    //       preparation and the time of approval.
    int anchorHeight;

public:
    TransactionEffects(
        AccountId sendFromAccount,
        uint32_t anchorConfirmations,
        SpendableInputs spendable,
        Payments payments,
        std::optional<ChangeAddress> changeAddr,
        CAmount fee,
        uint256 internalOVK,
        uint256 externalOVK,
        int anchorHeight) :
            sendFromAccount(sendFromAccount),
            anchorConfirmations(anchorConfirmations),
            spendable(spendable),
            payments(payments),
            changeAddr(changeAddr),
            fee(fee),
            internalOVK(internalOVK),
            externalOVK(externalOVK),
            anchorHeight(anchorHeight) {}

    PrivacyPolicy GetRequiredPrivacyPolicy() const;

    const SpendableInputs& GetSpendable() const {
        return spendable;
    }

    const Payments& GetPayments() const {
        return payments;
    }

    CAmount GetFee() const {
        return fee;
    }

    bool InvolvesOrchard() const;

    TransactionBuilderResult ApproveAndBuild(
            const Consensus::Params& consensus,
            const CWallet& wallet,
            const CChain& chain,
            const TransactionStrategy& strategy) const;
};

enum class AddressResolutionError {
    SproutSpendNotPermitted,
    SproutRecipientNotPermitted,
    TransparentRecipientNotPermitted,
    InsufficientSaplingFunds,
    UnifiedAddressResolutionError,
    ChangeAddressSelectionError
};

class InsufficientFundsError {
public:
    CAmount required;

    InsufficientFundsError(CAmount required):
        required(required) { }
};

class DustThresholdError {
public:
    CAmount dustThreshold;
    CAmount changeAmount;

    DustThresholdError(CAmount dustThreshold, CAmount changeAmount):
        dustThreshold(dustThreshold), changeAmount(changeAmount) { }
};

typedef std::variant<
    InsufficientFundsError,
    DustThresholdError> InvalidFundsReason;

class InvalidFundsError {
public:
    CAmount available;
    bool transparentCoinbasePermitted;
    const InvalidFundsReason reason;

    InvalidFundsError(CAmount available, bool transparentCoinbasePermitted, const InvalidFundsReason reason):
        available(available), transparentCoinbasePermitted(transparentCoinbasePermitted), reason(reason) { }
};

class ChangeNotAllowedError {
public:
    CAmount available;
    CAmount required;

    ChangeNotAllowedError(CAmount available, CAmount required):
        available(available), required(required) { }
};

class ExcessOrchardActionsError {
public:
    uint32_t orchardNotes;
    uint32_t maxNotes;

    ExcessOrchardActionsError(uint32_t orchardNotes, uint32_t maxNotes): orchardNotes(orchardNotes), maxNotes(maxNotes) { }
};

typedef std::variant<
    AddressResolutionError,
    InvalidFundsError,
    ChangeNotAllowedError,
    ExcessOrchardActionsError> InputSelectionError;

class InputSelection {
private:
    Payments payments;
    int orchardAnchorHeight;

public:
    InputSelection(Payments payments, int orchardAnchorHeight):
        payments(payments), orchardAnchorHeight(orchardAnchorHeight) {}

    Payments GetPayments() const;
};

typedef std::variant<
    InputSelectionError,
    InputSelection> InputSelectionResult;

typedef std::variant<
    InputSelectionError,
    TransactionEffects> PrepareTransactionResult;

class WalletTxBuilder {
private:
    const CChainParams& params;
    const CWallet& wallet;
    CFeeRate minRelayFee;
    uint32_t maxOrchardActions;

    /**
     * Compute the default dust threshold
     */
    CAmount DefaultDustThreshold() const;

    /**
     * Select inputs sufficient to fulfill the specified requested payments,
     * and choose unified address receivers based upon the available inputs
     * and the requested transaction strategy.
     */
    InputSelectionResult ResolveInputsAndPayments(
            const ZTXOSelector& selector,
            SpendableInputs& spendable,
            const std::vector<Payment>& payments,
            const CChain& chain,
            TransactionStrategy strategy,
            CAmount fee,
            int anchorHeight) const;
    /**
     * Compute the internal and external OVKs to use in transaction construction, given
     * the spendable inputs.
     */
    std::pair<uint256, uint256> SelectOVKs(
            const ZTXOSelector& selector,
            const SpendableInputs& spendable) const;

public:
    WalletTxBuilder(const CChainParams& params, const CWallet& wallet, CFeeRate minRelayFee):
        params(params), wallet(wallet), minRelayFee(minRelayFee), maxOrchardActions(nOrchardActionLimit) {}

    static bool AllowTransparentCoinbase(
            const std::vector<Payment>& payments,
            TransactionStrategy strategy);

    SpendableInputs FindAllSpendableInputs(
            const ZTXOSelector& selector,
            bool allowTransparentCoinbase,
            int32_t minDepth) const;

    PrepareTransactionResult PrepareTransaction(
            const ZTXOSelector& selector,
            SpendableInputs& spendable,
            const std::vector<Payment>& payments,
            const CChain& chain,
            TransactionStrategy strategy,
            CAmount fee,
            uint32_t anchorConfirmations) const;
};

#endif
