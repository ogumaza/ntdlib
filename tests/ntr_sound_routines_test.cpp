// SPDX-License-Identifier: MIT

// Tests of the search for the SDK's sound routines (ntdlib/ntr/sound_routines.h), on a binary that the ARM assembler
// writes: the routines in the shapes that the SDK's builds have, and near misses that the rules leave out. The Thumb
// routines, the ex form of the start, OS_WaitIrq and the command routine have the instruction words of the games
// that the rules were written for: Pokémon Black 2 (USA), Pokémon HeartGold (USA), Mario Kart DS (USA) and Pokémon
// Black (USA).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "ntdlib/arm/assembler.h"
#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/sound_routines.h"
#include "thumb_code.h"

namespace
{

using ntdlib::arm::ArmAssembler;
using ntdlib::arm::Condition;
using ntdlib::testing::ThumbCode;
namespace ntr = ntdlib::ntr;
namespace testing = ntdlib::testing;

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
    XSFLIB_CHECK(found.waits == std::vector<ntdlib::ntr::SoundWait>{{.address = binary.At("wait")}});
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

// The sound library in Thumb code, as Pokémon Black 2 (USA) builds it: the update (0x0206B930), the start
// (0x0206D974), the load's wrapper (0x0206D274), the wait (0x0207C1E8) and the command routine (0x0207D648), each with
// the game's halfwords. The leaves are bx lr, OS_WaitIrq in ARM code.
std::vector<uint8_t> ThumbSoundLibrary(ThumbCode& t)
{
    using testing::ThumbCopy;
    using testing::ThumbMovs;

    t.Bind("update");
    t.Half(testing::ThumbPush(0x10));
    t.Half(ThumbMovs(4, 0));
    t.Bind("receive_loop");
    t.Half(ThumbCopy(0, 4));
    t.Bl("receive");
    t.Half(testing::ThumbCmp(0, 0));
    t.B(testing::kThumbNe, "receive_loop");
    t.Bl("a");
    t.Bl("b");
    t.Bl("c");
    t.Half(ThumbMovs(0, 0));
    t.Bl("flush");
    t.Half(testing::ThumbPop(0x10));

    t.Bind("start");
    t.Half(testing::ThumbPush(0x38));
    t.Half(testing::ThumbSubSp(8));
    t.Half(ThumbCopy(4, 1));
    t.Half(ThumbCopy(5, 0));
    t.Half(ThumbCopy(0, 4));
    t.Bl("get_info");
    t.Half(ThumbCopy(3, 0));
    t.B(testing::kThumbNe, "start_record");
    t.Half(testing::ThumbAddSp(8));
    t.Half(ThumbMovs(0, 0));
    t.Half(testing::ThumbPop(0x38));
    t.Bind("start_record");
    t.Half(testing::ThumbStrSp(3, 0));
    t.Half(testing::ThumbStrSp(4, 4));
    t.Half(testing::ThumbLdrb(1, 3, 9));
    t.Half(testing::ThumbLdrh(2, 3, 4));
    t.Half(testing::ThumbLdrb(3, 3, 8));
    t.Half(ThumbCopy(0, 5));
    t.Bl("start_inner");
    t.Half(testing::ThumbAddSp(8));
    t.Half(testing::ThumbPop(0x38));

    t.Bind("load");
    t.Half(testing::ThumbPush(0x08));
    t.Half(ThumbCopy(2, 1));
    t.Half(ThumbMovs(1, 0));
    t.Half(testing::ThumbStrSp(1, 0));
    t.Half(ThumbMovs(1, 0xFF));
    t.Half(ThumbMovs(3, 1));
    t.Bl("load_inner");
    t.Half(testing::ThumbCmp(0, 0));
    t.B(testing::kThumbNe, "load_failed");
    t.Half(ThumbMovs(0, 1));
    t.Half(testing::ThumbPop(0x08));
    t.Bind("load_failed");
    t.Half(ThumbMovs(0, 0));
    t.Half(testing::ThumbPop(0x08));

    // The load's callee reads the record and passes its other arguments on to the bank's load.
    t.Bind("load_inner");
    t.Half(testing::ThumbPush(0xF0));
    t.Half(ThumbCopy(5, 1));
    t.Half(ThumbCopy(6, 2));
    t.Half(ThumbCopy(7, 3));
    t.Bl("get_info");
    t.Half(testing::ThumbLdrh(0, 0, 4));
    t.Half(ThumbCopy(1, 5));
    t.Half(ThumbCopy(2, 6));
    t.Half(ThumbCopy(3, 7));
    t.Bl("load_bank");
    t.Half(testing::ThumbPop(0xF0));

    t.Bind("wait");
    t.Half(testing::ThumbPush(0x08));
    t.Half(ThumbMovs(0, 1));
    t.Bl("wait_by_loop");
    t.Half(ThumbMovs(0, 1));
    t.Half(ThumbMovs(1, 1));
    t.Blx("wait_irq");
    t.Half(testing::ThumbPop(0x08));

    t.Bind("command");
    t.Half(testing::ThumbPush(0xF8));
    t.Half(ThumbCopy(5, 0));
    t.Half(ThumbMovs(0, 1));
    t.Half(ThumbCopy(4, 1));
    t.Half(ThumbCopy(6, 2));
    t.Half(ThumbCopy(7, 3));
    t.Bl("allocate");
    t.Half(testing::ThumbCmp(0, 0));
    t.B(testing::kThumbEq, "command_end");
    t.Half(testing::ThumbStr(5, 0, 4));
    t.Half(testing::ThumbStr(4, 0, 8));
    t.Half(testing::ThumbStr(6, 0, 12));
    t.Half(testing::ThumbLdrSp(1, 0x18));
    t.Half(testing::ThumbStr(7, 0, 16));
    t.Half(testing::ThumbStr(1, 0, 20));
    t.Bl("queue");
    t.Bind("command_end");
    t.Half(testing::ThumbPop(0xF8));

    for (const char* leaf : {"receive", "a", "b", "c", "flush", "get_info", "start_inner", "load_bank", "wait_by_loop",
                             "allocate", "queue"})
    {
        t.Bind(leaf);
        t.Half(testing::kThumbBxLr);
    }
    t.Align();
    t.Bind("wait_irq");
    t.Half(0xFF1E); // bx lr in ARM code
    t.Half(0xE12F);

    return t.Finish();
}

// The search reads Thumb code: each routine of the Thumb library, with bit 0 set.
void TestThumbLibrary()
{
    ThumbCode t(kBase);
    const ntr::LoadedCode code{.address = kBase, .bytes = ThumbSoundLibrary(t)};
    const auto thumb = [&t](const char* label)
    {
        return t.At(label) | 1;
    };
    const ntr::SoundUpdate update{.address = thumb("update"),
                                  .receive = thumb("receive"),
                                  .calls = {thumb("a"), thumb("b"), thumb("c")},
                                  .flush = thumb("flush")};
    const ntr::SequenceStart start{
        .address = thumb("start"), .get_info = thumb("get_info"), .start = thumb("start_inner")};
    const ntr::SequenceLoad load{.wrapper = thumb("load"), .load = thumb("load_inner")};

    const ntr::SoundRoutines found = ntr::FindSoundRoutines(code, kBase);

    XSFLIB_CHECK(found.updates == std::vector<ntr::SoundUpdate>{update});
    XSFLIB_CHECK(found.starts == std::vector<ntr::SequenceStart>{start});
    XSFLIB_CHECK(found.loads == std::vector<ntr::SequenceLoad>{load});
    XSFLIB_CHECK(found.waits == std::vector<ntr::SoundWait>{{.address = thumb("wait")}});
    XSFLIB_CHECK(found.commands == std::vector<uint32_t>{thumb("command")});
}

// NNS_SndArcPlayerStartSeqEx in ARM code, with Pokémon HeartGold's words (0x020CA8E4). `plain` adds a plain start. The
// search then prefers that start. Without `conditional`, the player's load always runs: a near miss.
struct ExBinary
{
    ntr::LoadedCode code;
    uint32_t start_ex = 0;
    uint32_t plain = 0;
    uint32_t get_info = 0;
    uint32_t start_inner = 0;
};

ExBinary StartExBinary(bool plain, bool conditional)
{
    constexpr uint32_t kAddeqSp8 = 0x028DD008;
    constexpr uint32_t kPopeqR3ToR7Pc = 0x08BD80F8;
    constexpr uint32_t kLdrbltR4Priority = 0xB5D04008;
    constexpr uint32_t kLdrhltR5Bank = 0xB1D050B4;
    constexpr uint32_t kLdrbltR6Player = 0xB5D06009;
    constexpr uint32_t kLdrbR6Player = 0xE5D06009;

    ArmAssembler a(kBase);
    const int get_info = a.NewLabel();
    const int start_inner = a.NewLabel();
    ExBinary binary;
    binary.start_ex = a.Here();
    a.Push(0x40F8);
    a.Sub(13, 13, 8);
    a.MovRegister(7, 0);
    a.Ldr(0, 13, 0x20);
    a.MovRegister(6, 1);
    a.MovRegister(5, 2);
    a.MovRegister(4, 3);
    a.Bl(get_info);
    a.Cmp(0, 0);
    a.Word(kAddeqSp8);
    a.Word(kMoveqR0);
    a.Word(kPopeqR3ToR7Pc);
    a.Cmp(4, 0);
    a.Word(kLdrbltR4Priority);
    a.Cmp(5, 0);
    a.Word(kLdrhltR5Bank);
    a.Cmp(6, 0);
    a.Word(conditional ? kLdrbltR6Player : kLdrbR6Player);
    a.Ldr(12, 13, 0x20);
    a.MovRegister(2, 5);
    a.Str(0, 13, 0);
    a.MovRegister(0, 7);
    a.MovRegister(1, 6);
    a.MovRegister(3, 4);
    a.Str(12, 13, 4);
    a.Bl(start_inner);
    a.Add(13, 13, 8);
    a.Pop(0x80F8);
    if (plain)
    {
        binary.plain = a.Here();
        a.Push(0x4038);
        a.MovRegister(4, 1);
        a.MovRegister(5, 0);
        a.MovRegister(0, 4);
        a.Bl(get_info);
        a.Ldrb(1, 0, 9);
        a.Ldrh(2, 0, 4);
        a.Ldrb(3, 0, 8);
        a.MovRegister(0, 5);
        a.Bl(start_inner);
        a.Pop(0x8038);
    }
    binary.get_info = a.Here();
    a.Bind(get_info);
    a.Bx(14);
    binary.start_inner = a.Here();
    a.Bind(start_inner);
    a.Bx(14);
    binary.code = {.address = kBase, .bytes = a.Finish()};

    return binary;
}

// The ex form counts only where no plain start matches, and only with every field read under its condition.
void TestStartEx()
{
    const ExBinary alone = StartExBinary(false, true);
    const ExBinary with_plain = StartExBinary(true, true);
    const ExBinary unconditional = StartExBinary(false, false);

    const ntr::SoundRoutines found_alone = ntr::FindSoundRoutines(alone.code, kBase);
    const ntr::SoundRoutines found_with_plain = ntr::FindSoundRoutines(with_plain.code, kBase);
    const ntr::SoundRoutines found_unconditional = ntr::FindSoundRoutines(unconditional.code, kBase);

    const ntr::SequenceStart ex{
        .address = alone.start_ex, .get_info = alone.get_info, .start = alone.start_inner, .ex = true};
    const ntr::SequenceStart plain{
        .address = with_plain.plain, .get_info = with_plain.get_info, .start = with_plain.start_inner};
    XSFLIB_CHECK(found_alone.starts == std::vector<ntr::SequenceStart>{ex});
    XSFLIB_CHECK(found_with_plain.starts == std::vector<ntr::SequenceStart>{plain});
    XSFLIB_CHECK(found_unconditional.starts.empty());
}

// OS_WaitIrq in ARM code, with Mario Kart DS's words (0x0200C894) and the interrupt check word at `check`.
void WaitIrq(ArmAssembler& a, uint32_t check)
{
    constexpr uint32_t kBeqStore = 0x0A000005;    // beq to the routine's second call
    constexpr uint32_t kMvnR2R4 = 0xE1E02004;     // mvn r2, r4
    constexpr uint32_t kLoadCheck = 0xE5913FF8;   // ldr r3, [r1, #0xFF8]
    constexpr uint32_t kAndR2R3R2 = 0xE0032002;   // and r2, r3, r2
    constexpr uint32_t kStoreCheck = 0xE5812FF8;  // str r2, [r1, #0xFF8]
    constexpr uint32_t kLoadCheckR0 = 0xE5900FF8; // ldr r0, [r0, #0xFF8]
    constexpr uint32_t kAndsR0R4R0 = 0xE0140000;  // ands r0, r4, r0
    constexpr uint32_t kPopneR4R5R6Lr = 0x18BD4070;
    constexpr uint32_t kBxneLr = 0x112FFF1E;
    constexpr uint32_t kHigh = 0x3000;

    const int leaf = a.NewLabel();
    a.Push(0x4070);
    a.MovRegister(5, 0);
    a.MovRegister(4, 1);
    a.Bl(leaf);
    a.Cmp(5, 0);
    a.Word(kBeqStore);
    a.LoadConstant(1, check - kHigh - 0xFF8);
    a.Word(kMvnR2R4);
    a.Add(1, 1, kHigh);
    a.Word(kLoadCheck);
    a.Word(kAndR2R3R2);
    a.Word(kStoreCheck);
    a.Bl(leaf);
    a.LoadConstant(1, check - kHigh - 0xFF8);
    a.Add(0, 1, kHigh);
    a.Word(kLoadCheckR0);
    a.Word(kAndsR0R4R0);
    a.Word(kPopneR4R5R6Lr);
    a.Word(kBxneLr);
    a.MovRegister(0, 5);
    a.Bl(leaf);
    a.Pop(0x8070);
    a.Bind(leaf);
    a.Bx(14);
}

// Without OS_WaitVBlankIntr, OS_WaitIrq is the wait, with 1 and 1. A routine of the same shape on another word of the
// DTCM isn't.
void TestWaitIrq()
{
    ArmAssembler a(kBase);
    const uint32_t wait = a.Here();
    WaitIrq(a, 0x027E3FF8);
    WaitIrq(a, 0x027E3FF4);
    const ntr::LoadedCode code{.address = kBase, .bytes = a.Finish()};

    const ntr::SoundRoutines found = ntr::FindSoundRoutines(code, kBase);

    const std::vector<ntr::SoundWait> expected = {{.address = wait, .arguments = {1, 1}}};
    XSFLIB_CHECK(found.waits == expected);
}

// The command routine in ARM code, with Pokémon Black's words (0x02089804). A routine that stores the fifth argument
// at 16 in place of the fourth is a near miss.
void TestCommand()
{
    constexpr uint32_t kPopeqR3ToR7Pc = 0x08BD80F8;
    const auto command = [](ArmAssembler& a, int last_field)
    {
        const int leaf = a.NewLabel();
        a.Push(0x40F8);
        a.MovRegister(7, 0);
        a.Mov(0, 1);
        a.MovRegister(6, 1);
        a.MovRegister(5, 2);
        a.MovRegister(4, 3);
        a.Bl(leaf);
        a.Cmp(0, 0);
        a.Word(kPopeqR3ToR7Pc);
        a.Str(7, 0, 4);
        a.Str(6, 0, 8);
        a.Str(5, 0, 12);
        a.Ldr(1, 13, 0x18);
        a.Str(4, 0, last_field == 20 ? 16 : 20);
        a.Str(1, 0, last_field);
        a.Bl(leaf);
        a.Pop(0x80F8);
        a.Bind(leaf);
        a.Bx(14);
    };
    ArmAssembler a(kBase);
    const uint32_t routine = a.Here();
    command(a, 20);
    command(a, 16);
    const ntr::LoadedCode code{.address = kBase, .bytes = a.Finish()};

    const ntr::SoundRoutines found = ntr::FindSoundRoutines(code, kBase);

    XSFLIB_CHECK(found.commands == std::vector<uint32_t>{routine});
}

} // namespace

int main()
{
    TestFindsEachRoutine();
    TestNearMisses();
    TestLoadWithoutWrapper();
    TestUpdateCalls();
    TestThumbLibrary();
    TestStartEx();
    TestWaitIrq();
    TestCommand();

    return xsflib::testing::Summary("DS sound routines");
}
