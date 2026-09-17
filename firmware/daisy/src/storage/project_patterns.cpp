#include "project_patterns.hpp"

#include "pattern_store.hpp"
#include "wxcf/pattern_file.hpp"
#include <cstdio>
#include <cstring>

namespace WaveX::Storage {
using namespace Protocol;
using namespace Sequencer;
using State = PatternExchange::State;
void ProjectPatterns::Refresh(const Project* project) {
    status_.active_pattern = project ? project->active_pattern : 0;
    status_.used =
        status_.slot == status_.active_pattern || (project && project->patterns[status_.slot].used);
    const char* name = project ? project->patterns[status_.slot].name : "";
    if (status_.slot == status_.active_pattern && PatternStore::CurrentName()[0])
        name = PatternStore::CurrentName();
    detail::CopyWireString(status_.name, sizeof(status_.name), status_.used ? name : "");
    if (status_.used && !status_.name[0])
        std::snprintf(status_.name, sizeof(status_.name), "Pattern %u", status_.slot + 1);
}
bool ProjectPatterns::Request(const SeqSlotOpMessage& request, const Project* project, bool busy) {
    if (!IsValidSeqSlotOp(request))
        return false;
    status_.request_id = request.request_id;
    status_.slot = request.slot;
    reply_ = true;
    Refresh(project);
    if (request.op == SEQ_SLOT_GET || request.request_id == status_.active_request_id ||
        request.request_id == status_.completed_request_id)
        return false;
    uint8_t error = SEQ_SLOT_OK;
    if (Busy() || busy || exchange_.state() != State::Idle)
        error = SEQ_SLOT_BUSY;
    else if (request.op != SEQ_SLOT_SELECT && request.op != SEQ_SLOT_LAUNCH &&
             !PatternFile::ValidName(request.name))
        error = SEQ_SLOT_BAD_NAME;
    else if ((request.op == SEQ_SLOT_CREATE || request.op == SEQ_SLOT_COPY) && status_.used)
        error = SEQ_SLOT_EXISTS;
    else if ((request.op == SEQ_SLOT_RENAME || request.op == SEQ_SLOT_SELECT ||
              request.op == SEQ_SLOT_LAUNCH) &&
             !status_.used)
        error = SEQ_SLOT_EMPTY;
    if (error != SEQ_SLOT_OK) {
        status_.completed_request_id = request.request_id;
        status_.completed_op = request.op;
        status_.error = error;
        return false;
    }
    request_ = request;
    status_.active_request_id = request.request_id;
    status_.busy = 1;
    status_.error = SEQ_SLOT_OK;
    if (request.op == SEQ_SLOT_LAUNCH) {
        if (request.slot == status_.active_pattern) {
            Fail(SEQ_SLOT_OK);
            return false;
        }
        exchange_.foreground() = project->patterns[request.slot].pattern;
        exchange_.Launch(request.slot);
        status_.queued_pattern = request.slot;
        phase_ = Phase::Launch;
    } else {
        phase_ = Phase::Capture;
        exchange_.Capture(true);
    }
    return true;
}
void ProjectPatterns::Fail(uint8_t error) {
    exchange_.Retire();
    phase_ = Phase::Idle;
    status_.busy = 0;
    status_.active_request_id = 0;
    status_.queued_pattern = 0xff;
    status_.completed_request_id = request_.request_id;
    status_.completed_op = request_.op;
    status_.error = error;
    reply_ = true;
}
void ProjectPatterns::Pump(Project* project) {
    if (!Busy())
        return;
    if (phase_ == Phase::Launch) {
        if (exchange_.state() == State::Cancelled) {
            Fail(SEQ_SLOT_CANCELLED);
            return;
        }
        if (exchange_.state() == State::Running) {
            Fail(SEQ_SLOT_STOP_FIRST);
            return;
        }
        if (exchange_.state() != State::Launched)
            return;
        auto& outgoing = project->patterns[project->active_pattern];
        outgoing.used = true;
        outgoing.pattern = exchange_.foreground();
        const char* name = PatternStore::CurrentName();
        if (name[0])
            detail::CopyWireString(outgoing.name, sizeof(outgoing.name), name);
        if (!outgoing.name[0])
            std::snprintf(
                outgoing.name, sizeof(outgoing.name), "Pattern %u", project->active_pattern + 1);
        project->active_pattern = request_.slot;
        PatternStore::SetProjectPatternName(project->patterns[request_.slot].name);
        Fail(SEQ_SLOT_OK);
        Refresh(project);
        return;
    }
    if (phase_ == Phase::Install) {
        if (exchange_.state() != State::Installed)
            return;
        project->active_pattern = request_.slot;
        PatternStore::SetProjectPatternName(project->patterns[request_.slot].name);
        Fail(SEQ_SLOT_OK);
        Refresh(project);
        return;
    }
    if (exchange_.state() == State::Running) {
        Fail(SEQ_SLOT_STOP_FIRST);
        return;
    }
    if (exchange_.state() == State::Failed) {
        Fail(SEQ_SLOT_CAPTURE_BUSY);
        return;
    }
    if (exchange_.state() != State::Captured)
        return;
    if (!project) {
        Fail(SEQ_SLOT_NO_MEMORY);
        return;
    }
    auto& active = project->patterns[project->active_pattern];
    active.used = true;
    active.pattern = exchange_.foreground();
    const char* name = PatternStore::CurrentName();
    if (name[0])
        detail::CopyWireString(active.name, sizeof(active.name), name);
    if (!active.name[0])
        std::snprintf(active.name, sizeof(active.name), "Pattern %u", project->active_pattern + 1);
    exchange_.Retire();
    auto& target = project->patterns[request_.slot];
    if (request_.op == SEQ_SLOT_SELECT && request_.slot != project->active_pattern) {
        exchange_.foreground() = target.pattern;
        exchange_.Install(request_.slot);
        phase_ = Phase::Install;
        return;
    }
    if (request_.op == SEQ_SLOT_CREATE) {
        target.pattern = Pattern{};
        target.used = true;
    }
    if (request_.op == SEQ_SLOT_COPY) {
        target.pattern = active.pattern;
        target.used = true;
    }
    if (request_.op != SEQ_SLOT_SELECT) {
        detail::CopyWireString(target.name, sizeof(target.name), request_.name);
        if (request_.slot == project->active_pattern)
            PatternStore::SetProjectPatternName(target.name);
    }
    Fail(SEQ_SLOT_OK);
    Refresh(project);
}
}  // namespace WaveX::Storage
