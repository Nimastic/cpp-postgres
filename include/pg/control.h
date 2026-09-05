#pragma once

#include "pg/constants.h"
#include "pg/file.h"
#include "pg/tuple.h"
#include <string>
#include <cstring>

namespace pg {

using lsn_t_fwd = uint64_t;

enum class DBState : uint32_t {
    SHUTDOWNED  = 0,  // Last shutdown was clean; recovery is not needed
    IN_PRODUCTION = 1 // Opened for writes and not yet closed; a crash leaves this behind
};

#pragma pack(push, 1)
// The control file. PostgreSQL keeps the equivalent in global/pg_control and
// reads it before anything else at startup.
//
// Two things live here that cannot be re-derived from the data files:
//
//  * whether the last shutdown was clean. Without it there is no way to know
//    that recovery is needed, and a crashed database quietly reopens in an
//    unrecovered state.
//  * the transaction id counter. Re-deriving it from the highest xid still
//    visible in the heap reissues ids whose commit status is already recorded
//    in the commit log, so visibility starts resolving against the wrong
//    transaction; vacuum away every old row and the counter restarts at 1.
struct ControlData {
    uint32_t magic{0x50474354};        // 'PGCT'
    uint32_t version{1};
    uint32_t page_size{static_cast<uint32_t>(PAGE_SIZE)};
    DBState  state{DBState::SHUTDOWNED};
    tx_id_t  next_xid{1};
    uint64_t checkpoint_lsn{0};
    uint64_t checkpoint_redo{0};       // Where recovery must start replaying
    uint32_t crc{0};

    uint32_t compute_crc() const;
};
#pragma pack(pop)

class ControlFile {
public:
    explicit ControlFile(const std::string& path);

    const ControlData& data() const { return data_; }
    ControlData& data() { return data_; }

    // True when the file was missing, corrupt, or marked IN_PRODUCTION -- i.e.
    // the previous run did not shut down cleanly and redo must run.
    bool needs_recovery() const { return needs_recovery_; }

    // Persist and fsync. The control file is the one thing that must be durable
    // before anything trusts it, so every write goes straight to stable storage.
    void save();

    void mark_in_production();
    void mark_shutdown();

private:
    std::string path_;
    File file_;
    ControlData data_{};
    bool needs_recovery_{false};
};

} // namespace pg
