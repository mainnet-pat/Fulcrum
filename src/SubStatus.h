//
// Fulcrum - A fast & nimble SPV Server for Bitcoin Cash
// Copyright (C) 2019-2021  Calin A. Culianu <calin.culianu@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program (see LICENSE.txt).  If not, see
// <https://www.gnu.org/licenses/>.
//
#pragma once

#include "Util.h"
#include "DSProof.h"

#include "bitcoin/hash.h"

#include <QByteArray>
#include <QMetaType>
#include <QVariant>

#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility> // for move

/// This class is sort of like a variant/optional combination. It can store either "No Value" (!.has_value()) or
/// either a: QByteArray, DSProof or a std::optional<BlockHeight>. It is optimized for minimal memory usage in the
/// common case -- taking up as much memory as a std::optional<QByteArray> (16 bytes on 64-bit).
///
/// It is intended to be used with the SubsMgr and its subclasses.
///
/// - ScriptHashSubsMgr::getFullStatus() always returns one of these objects with the QByteArray as the active value,
///   that is, byteArray() will always be valid pointer.
///
/// - DSProofSubsMgr::getFullStatus() always returns one of these objects with the DSProof as the active value,
///   that is, dsproof() will always be a valid pointer.
///
/// - TransactionSubsMgr::getFullStatus() always returns one of these objects with the std::optional<BlockHeight> as
///   the active value, that is, blockHeight() will always be a valid pointer (even if it itself !has_value()).
///
/// - ScriptHashTransactionSubsMgr::getFullStatus() always returns one of these objects with the std::optional<HashSet> as
///   the active value, that is, hashSet() will always be a valid pointer (even if it itself !has_value()).
///

using HashSet = std::set<HashX>;

namespace {
    // assumption: `hashSet` is not empty!
    // set is ordered to prevent different outcome of the same element composition
    inline QByteArray HashSetHash(const HashSet &hashSet) {
//        static_assert (sizeof(decltype(hashSet.front())) <= 4, "Assumption below is for at most 32-bit heights");
        constexpr size_t WorstCaseElementSize = HashLen*2 + 11 + 2; // worse case: 11 bytes max for sign & int, 2 colons, plus 64 bytes for hashHex
        bitcoin::CHash256 hasher(true /* hash once */);
        for (const auto & hash : hashSet) {
            constexpr size_t BufSize = WorstCaseElementSize + 10; // leave a little room (this happens to align sbuf to cache on 64-bit)
            Util::AsyncSignalSafe::SBuf<BufSize> sbuf; // fast stack-based buffer
            if (const auto hexLen = hash.length() * 2; LIKELY(hexLen <= HashLen * 2)) {
                Util::ToHexFastInPlace(hash, sbuf.strBuf.data(), hexLen);
                sbuf.len += hexLen;
            }
            hasher.Write(reinterpret_cast<const uint8_t *>(std::as_const(sbuf.strBuf).data()), sbuf.len);
        }

        static_assert (hasher.OUTPUT_SIZE == HashLen, "Assumption is that HashLen is the sha256 output size (32 bytes)");
        QByteArray ret{HashLen, Qt::Uninitialized};
        hasher.Finalize(reinterpret_cast<uint8_t *>(ret.data()));

        // status is non-reversed, single sha256 (32 bytes)
        return ret;
    }
}

class SubStatus {
    union {
        QByteArray qba;
        std::unique_ptr<DSProof> dsp; // we use unique_ptr here to save memory in the common case where most of these instances are QByteArray
        std::optional<BlockHeight> bh; // !has_value indicates unknown txhash, otherwise 0 = mempool, >0 = confirmed height
        std::optional<HashSet> hs; // hashset, !has_value indicates unknown txhash, otherwise 0 = mempool, >0 = confirmed height
        void *dummy = nullptr;
    };
    enum T : uint8_t { NoValue, QBA, DSP, BH, HS };
    T t = NoValue;
    void destruct() {
        switch (t) {
        case NoValue: return;
        case QBA: qba.~QByteArray(); break;
        case DSP: dsp.~unique_ptr(); break;
        case BH : bh.~optional(); break;
        case HS : hs.~optional(); break;
        }
        dummy = nullptr;
        t = NoValue;
    }
    void construct(const T tt) {
        destruct();
        switch (tt) {
        case NoValue: return;
        case QBA:
            new (&qba) QByteArray;
            break;
        case DSP:
            new (&dsp) std::unique_ptr<DSProof>(new DSProof);
            break;
        case BH:
            new (&bh) std::optional<BlockHeight>;
            break;
        case HS:
            new (&hs) std::optional<HashSet>;
        }
        t = tt;
    }
    void move(SubStatus &&o) {
        if (this == &o) return;
        if (t != o.t) {
            if (o.t == DSP) {
                // optimization for DSP: in the move case we want to avoid constructing a new DSProof, and
                // just transfer ownership of the DSProof owned by o.dsp
                destruct();
                new (&dsp) std::unique_ptr<DSProof>;
                t = DSP;
                // fall thru to below code to transfer pointer
            } else {
                // normal case: QBA, BH, or NoValue
                construct(o.t);
            }
        }
        // at this point t == o.t
        switch (t) {
        case NoValue: break; // nothing to do
        case QBA: qba = std::move(o.qba); break;
        case DSP: dsp = std::move(o.dsp); break; // cheap pointer transfer
        case BH: bh = std::move(o.bh); break;
        case HS: hs = std::move(o.hs); break;
        }
    }
    void copy(const SubStatus &o) {
        if (this == &o) return;
        if (t != o.t)
            construct(o.t);
        switch (t) {
        case NoValue: break;
        case QBA: qba = o.qba; break;
        case DSP: *dsp = *o.dsp; break;
        case BH: bh = o.bh; break;
        case HS: hs = o.hs; break;
        }
    }
public:
    constexpr SubStatus() noexcept {}
    SubStatus(SubStatus &&o) noexcept { move(std::move(o)); }
    SubStatus(const SubStatus &o) { copy(o); }
    SubStatus(const QByteArray &oq) noexcept : qba(oq), t{QBA} {}
    SubStatus(QByteArray &&oq) noexcept : qba(std::move(oq)), t{QBA} {}
    SubStatus(const DSProof &od) : dsp(new DSProof(od)), t{DSP} {}
    SubStatus(DSProof &&od) : dsp(new DSProof(std::move(od))), t{DSP} {}
    SubStatus(const std::optional<BlockHeight> &obh) noexcept : bh(obh), t{BH} {}
    SubStatus(const std::optional<HashSet> &ohs) noexcept : hs(ohs), t{HS} {}
    ~SubStatus() { destruct(); }

    SubStatus &operator=(const SubStatus &o) { copy(o); return *this; }
    SubStatus &operator=(SubStatus && o) { move(std::move(o)); return *this; }
    SubStatus &operator=(const QByteArray &oq) {
        if (t != QBA) construct(QBA);
        qba = oq;
        return *this;
    }
    SubStatus &operator=(QByteArray &&oq) {
        if (t != QBA) construct(QBA);
        qba = std::move(oq);
        return *this;
    }
    SubStatus &operator=(const DSProof &od) {
        if (t != DSP) construct(DSP);
        *dsp = od;
        return *this;
    }
    SubStatus &operator=(DSProof &&od) {
        if (t != DSP) construct(DSP);
        *dsp = std::move(od);
        return *this;
    }
    SubStatus &operator=(const std::optional<BlockHeight> &obh) {
        if (t != BH) construct(BH);
        bh = obh;
        return *this;
    }
    SubStatus &operator=(const std::optional<HashSet> &ohs) {
        if (t != HS) construct(HS);
        hs = ohs;
        return *this;
    }

   bool operator==(const SubStatus &o) const {
        if (t != o.t) return false;
        switch (t) { // t == o.t
        case NoValue: return true; // all NoValues are always equal
        case QBA: return qba == o.qba;
        case DSP: return *dsp == *o.dsp;
        case BH: return bh == o.bh;
        case HS: return hs == o.hs;
        }
    }
    bool operator!=(const SubStatus &o) const { return !(*this == o); }

    explicit operator bool() const noexcept { return has_value(); }

    bool has_value() const noexcept { return t != NoValue; }
    void reset() { destruct(); }

    QByteArray * byteArray() noexcept { return t == QBA ? &qba : nullptr; }
    const QByteArray * byteArray() const noexcept { return t == QBA ? &qba : nullptr; }

    DSProof * dsproof() noexcept { return t == DSP ? dsp.get() : nullptr; }
    const DSProof * dsproof() const noexcept { return t == DSP ? dsp.get() : nullptr; }

    const std::optional<BlockHeight> * blockHeight() const noexcept { return t == BH ? &bh : nullptr; }
    std::optional<BlockHeight> * blockHeight() noexcept { return t == BH ? &bh : nullptr; }

    const std::optional<HashSet> * hashSet() const noexcept { return t == HS ? &hs : nullptr; }
    std::optional<HashSet> * hashSet() noexcept { return t == HS ? &hs : nullptr; }

    /// Render this for JSON RPC (as a status result for notifications).  If !has_value() then it will be null,
    /// otherwise if it has a valid value it will be rendered as a string, or a dsproof object, or a number.
    /// Note that even if has_value(), this may still be a QVariant() (null).
    QVariant toVariant() const;
};

/// Specialization of std::hash so we can use SubStatus with std::unordered_map, std::unordered_set, etc
template <> struct std::hash<SubStatus> {
    std::size_t operator()(const SubStatus &s) const {
        if (auto *ba = s.byteArray(); ba) return HashHasher{}(*ba);
        else if (auto *dsp = s.dsproof(); dsp) return DspHash::Hasher{}(dsp->hash);
        else if (auto *bh = s.blockHeight(); bh && *bh) return Util::hashForStd(**bh);
        else if (auto *hs = s.hashSet(); hs && *hs) return HashHasher{}(HashSetHash(hs->value()));
        return 0; // !this->has_value() and/or !bh->has_value() hashes to 0 always
    }
};

Q_DECLARE_METATYPE(SubStatus);
