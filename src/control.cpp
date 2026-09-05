#include "pg/control.h"
#include <iostream>

namespace pg {

namespace {

uint32_t crc32_of(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
        }
    }
    return ~crc;
}

} // namespace

uint32_t ControlData::compute_crc() const {
    ControlData copy = *this;
    copy.crc = 0;
    return crc32_of(&copy, sizeof(ControlData));
}

ControlFile::ControlFile(const std::string& path)
    : path_(path), file_(File::open(path))
{
    if (file_.size() < sizeof(ControlData)) {
        // No control file yet: a fresh cluster, or one created before this
        // existed. Either way there is nothing to trust, so assume the worst
        // and let recovery decide.
        needs_recovery_ = true;
        data_ = ControlData{};
        return;
    }

    file_.read_at(0, &data_, sizeof(ControlData));

    if (data_.magic != 0x50474354 || data_.crc != data_.compute_crc()) {
        std::cerr << "Control file is corrupt; assuming crash recovery is required." << std::endl;
        needs_recovery_ = true;
        data_ = ControlData{};
        return;
    }

    if (data_.page_size != static_cast<uint32_t>(PAGE_SIZE)) {
        throw std::runtime_error("Control file page size mismatch: data directory was built with a different PAGE_SIZE");
    }

    // The decisive check. IN_PRODUCTION means the previous run never reached a
    // clean shutdown, so the heap may be ahead of, or behind, the log.
    needs_recovery_ = (data_.state == DBState::IN_PRODUCTION);
}

void ControlFile::save() {
    data_.crc = data_.compute_crc();
    file_.write_at(0, &data_, sizeof(ControlData));
    file_.sync();
}

void ControlFile::mark_in_production() {
    data_.state = DBState::IN_PRODUCTION;
    save();
}

void ControlFile::mark_shutdown() {
    data_.state = DBState::SHUTDOWNED;
    save();
}

} // namespace pg
