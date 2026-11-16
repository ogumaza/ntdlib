// SPDX-License-Identifier: MIT

// Tests of the search for the SDK's sound routines (ntdlib/ntr/sound_routines.h), on a binary that the ARM assembler
// writes: the routines in the shapes that the SDK's builds have, and near misses that the rules leave out.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/arm/assembler.h"
#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/sound_routines.h"

namespace
{

using ntdlib::arm::ArmAssembler;
using ntdlib::arm::Condition;
namespace ntr = ntdlib::ntr;

constexpr uint32_t kBase = 0x02000000;

// Instructions that the assembler doesn't write: ldr r0, [r1, #0x14], moveq r0, #0, ldmfdeq sp!, {r3, r4, r5, pc}
// and b to itself.
constexpr uint32_t kLoadCompressedEnd = 0xE5910014;
constexpr uint32_t kMoveqR0 = 0x03A00000;
constexpr uint32_t kPopeqR3R4R5Pc = 0x08BD8038;
constexpr uint32_t kLoop = 0xEAFFFFFE;

// blx to the Thumb code 8 bytes on.
constexpr uint32_t kBlxThumb = 0xFA000000;

// A binary with each routine, and the addresses of its labels by name.
class Binary
{
public:
    Binary() : a_(kBase)
    {
        for (const char* name : {"receive", "a", "b", "c", "flush", "get_info", "start_inner", "load_inner",
                                 "other_inner", "other", "load_bank", "wait_irq", "decompressor", "exit"})
        {
            labels_[name] = a_.NewLabel();
        }
    }

    // The entry code that calls the decompressor, and the decompressor that returns at once for 0.
    void Startup()
    {
        a_.Word(kLoadCompressedEnd);
        a_.Bl(labels_["decompressor"]);
        a_.Word(kLoop);
        Bind("decompressor");
        a_.Cmp(0, 0);
        a_.B(labels_["exit"], Condition::kEq);
        a_.MovRegister(0, 0);
        a_.MovRegister(0, 0);
        Bind("exit");
        a_.Bx(14);
    }

    // The update, plain or with its receive in Thumb code. Two near misses: a load among the calls, and calls that
    // take the routine's first argument.
    enum class UpdateKind
    {
        kPlain,
        kThumb,
        kLoad,
        kArgument,
    };

    void Update(UpdateKind kind)
    {
        Start("update");
        a_.Push(0x4030);
        a_.MovRegister(5, 0);
        a_.Mov(4, 0);
        const int loop = a_.NewLabel();
        a_.Bind(loop);
        a_.MovRegister(0, 4);
        if (kind == UpdateKind::kThumb)
        {
            thumb_receive_ = a_.Here() + 8 + 4 * (kBlxThumb & 0xFF);
            a_.Word(kBlxThumb);
        }
        else
        {
            a_.Bl(labels_["receive"]);
        }
        a_.Cmp(0, 0);
        a_.B(loop, Condition::kNe);
        if (kind == UpdateKind::kArgument)
        {
            a_.MovRegister(0, 5);
        }
        a_.Bl(labels_["a"]);
        if (kind == UpdateKind::kLoad)
        {
            a_.Ldr(0, 4, 0);
        }
        a_.Bl(labels_["b"]);
        a_.Bl(labels_["c"]);
        a_.Mov(0, 0);
        a_.Bl(labels_["flush"]);
        a_.Pop(0x8030);
    }

    // The start. Without `player`, the routine reads only two fields of the record: a near miss.
    void SequenceStart(bool player)
    {
        Start("start");
        a_.Push(0x4038);
        a_.MovRegister(4, 1);
        a_.MovRegister(5, 0);
        a_.MovRegister(0, 4);
        a_.Bl(labels_["get_info"]);
        a_.Cmp(0, 0);
        a_.Word(kMoveqR0);
        a_.Word(kPopeqR3R4R5Pc);
        if (player)
        {
            a_.Ldrb(1, 0, 9);
        }
        a_.Ldrh(2, 0, 4);
        a_.Ldrb(3, 0, 8);
        a_.MovRegister(0, 5);
        a_.Bl(labels_["start_inner"]);
        a_.Pop(0x8038);
    }

    // A load's wrapper that calls `inner` with (sequence, 0xFF, heap, 1, null).
    void LoadWrapper(const std::string& name, const std::string& inner)
    {
        Start(name);
        a_.Push(0x4008);
        a_.MovRegister(2, 1);
        a_.Mov(12, 0);
        a_.Mov(1, 0xFF);
        a_.Mov(3, 1);
        a_.Str(12, 13, 0);
        a_.Bl(labels_[inner]);
        a_.Pop(0x8008);
    }

    // The loads' callees: the sequence's reads the record and passes its other arguments on to the bank's load. The
    // other calls another routine. A third reads the record and loads nothing.
    void LoadInners()
    {
        Start("load_inner");
        a_.Push(0x40F0);
        a_.MovRegister(5, 1);
        a_.MovRegister(6, 2);
        a_.MovRegister(7, 3);
        a_.Bl(labels_["get_info"]);
        a_.Ldrh(0, 0, 4);
        a_.MovRegister(1, 5);
        a_.MovRegister(2, 6);
        a_.MovRegister(3, 7);
        a_.Bl(labels_["load_bank"]);
        a_.Pop(0x80F0);
        Start("other_inner");
        a_.Push(0x4010);
        a_.Bl(labels_["other"]);
        a_.Pop(0x8010);
        Start("parameter");
        a_.Push(0x4010);
        a_.Bl(labels_["get_info"]);
        a_.Add(0, 0, 4);
        a_.Pop(0x8010);
    }

    // The wait. Its first call goes to Thumb code, as the SDK's does.
    void Wait()
    {
        Start("wait");
        a_.Push(0x4008);
        a_.Mov(0, 1);
        a_.Word(kBlxThumb);
        a_.Mov(0, 1);
        a_.MovRegister(1, 0);
        a_.Bl(labels_["wait_irq"]);
        a_.Pop(0x8008);
    }

    // The routines that the others call, each a bx lr.
    void Leaves()
    {
        for (const char* name :
             {"receive", "a", "b", "c", "flush", "get_info", "start_inner", "other", "load_bank", "wait_irq"})
        {
            Bind(name);
            a_.Bx(14);
        }
    }

    ntr::LoadedCode Finish()
    {
        return {.address = kBase, .bytes = a_.Finish()};
    }

    uint32_t At(const std::string& name) const
    {
        return addresses_.at(name);
    }

    // The Thumb routine that the update's blx calls, with bit 0 set.
    uint32_t ThumbReceive() const
    {
        return thumb_receive_ | 1;
    }

private:
    void Bind(const std::string& name)
    {
        addresses_[name] = a_.Here();
        a_.Bind(labels_.at(name));
    }

    void Start(const std::string& name)
    {
        addresses_[name] = a_.Here();
        if (labels_.contains(name))
        {
            a_.Bind(labels_.at(name));
        }
    }

    ArmAssembler a_;
    std::map<std::string, int> labels_;
    std::map<std::string, uint32_t> addresses_;
    uint32_t thumb_receive_ = 0;
};

void TestFindsEachRoutine()
{
    Binary binary;
    binary.Startup();
    binary.Update(Binary::UpdateKind::kPlain);
    binary.SequenceStart(true);
    binary.LoadWrapper("load", "load_inner");
    binary.LoadWrapper("other_load", "other_inner");
    binary.LoadInners();
    binary.Wait();
    binary.Leaves();
    const ntr::LoadedCode code = binary.Finish();
    const ntr::SoundUpdate update{.address = binary.At("update"),
                                  .receive = binary.At("receive"),
                                  .calls = {binary.At("a"), binary.At("b"), binary.At("c")},
                                  .flush = binary.At("flush")};
    const ntr::SequenceStart start{
        .address = binary.At("start"), .get_info = binary.At("get_info"), .start = binary.At("start_inner")};
    const ntr::SequenceLoad load{.wrapper = binary.At("load"), .load = binary.At("load_inner")};

    const ntr::SoundRoutines found = ntr::FindSoundRoutines(code, kBase);

    XSFLIB_CHECK(found.updates == std::vector<ntr::SoundUpdate>{update});
    XSFLIB_CHECK(found.starts == std::vector<ntr::SequenceStart>{start});
    XSFLIB_CHECK(found.loads == std::vector<ntr::SequenceLoad>{load});
    XSFLIB_CHECK(found.waits == std::vector<uint32_t>{binary.At("wait")});
    XSFLIB_CHECK(found.decompressor && found.decompressor->routine == binary.At("decompressor"));
}

// An update with a load among its calls, and a start that reads two fields: neither matches, and without a start no
// load's wrapper counts.
void TestNearMisses()
{
    Binary binary;
    binary.Startup();
    binary.Update(Binary::UpdateKind::kLoad);
    binary.SequenceStart(false);
    binary.LoadWrapper("load", "load_inner");
    binary.LoadInners();
    binary.Leaves();
    const ntr::LoadedCode code = binary.Finish();

    const ntr::SoundRoutines found = ntr::FindSoundRoutines(code, kBase);

    XSFLIB_CHECK(found.updates.empty());
    XSFLIB_CHECK(found.starts.empty());
    XSFLIB_CHECK(found.loads.empty());
    XSFLIB_CHECK(found.waits.empty());
}

// A game without the load's wrapper: the routine that reads the sequence's record is the load.
void TestLoadWithoutWrapper()
{
    Binary binary;
    binary.SequenceStart(true);
    binary.LoadWrapper("other_load", "other_inner");
    binary.LoadInners();
    binary.Leaves();
    const ntr::LoadedCode code = binary.Finish();

    const ntr::SoundRoutines found = ntr::FindSoundRoutines(code, kBase);

    XSFLIB_CHECK(found.loads == std::vector<ntr::SequenceLoad>{{.load = binary.At("load_inner")}});
}

// An update whose receive is Thumb code, and one whose calls take the routine's argument: a near miss.
void TestUpdateCalls()
{
    Binary thumb;
    thumb.Update(Binary::UpdateKind::kThumb);
    thumb.Leaves();
    Binary argument;
    argument.Update(Binary::UpdateKind::kArgument);
    argument.Leaves();
    const ntr::LoadedCode thumb_code = thumb.Finish();
    const ntr::LoadedCode argument_code = argument.Finish();

    const ntr::SoundRoutines with_thumb = ntr::FindSoundRoutines(thumb_code, kBase);
    const ntr::SoundRoutines with_argument = ntr::FindSoundRoutines(argument_code, kBase);

    XSFLIB_CHECK(with_thumb.updates.size() == 1);
    XSFLIB_CHECK(!with_thumb.updates.empty() && with_thumb.updates[0].receive == thumb.ThumbReceive());
    XSFLIB_CHECK(with_argument.updates.empty());
}

} // namespace

int main()
{
    TestFindsEachRoutine();
    TestNearMisses();
    TestLoadWithoutWrapper();
    TestUpdateCalls();

    return xsflib::testing::Summary("DS sound routines");
}
