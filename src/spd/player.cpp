// SPDX-License-Identifier: MIT

// The addresses in the comments are those of the driver's code in Super Princess Peach (USA), on the ARM7. The model
// keeps the driver's layout of memory: every pointer is an address in the game's main memory, and the reads go to the
// blocks at the layout's addresses.

#include "ntdlib/spd/player.h"

#include "player_internal.h"

namespace ntdlib::spd
{

uint32_t StartMessage(int set, int sound, int fade)
{
    return static_cast<uint32_t>(MessageKind::kStart) | static_cast<uint32_t>(fade & 0xFF) << 8 |
           static_cast<uint32_t>(set & 0x3F) << 16 | static_cast<uint32_t>(sound & 0x3FF) << 22;
}

Layout GameLayout(std::span<const uint8_t> header)
{
    if (header.size() < kSlotTable + 8)
    {
        throw std::invalid_argument("the sound data's header is too short");
    }

    Layout layout;
    layout.area = kArea;
    layout.set_a = kArea + xsflib::LoadLe32(&header[kSlotTable]);
    layout.set_b = layout.set_a + xsflib::LoadLe32(&header[kSlotTable + 4]);
    return layout;
}

Player::Player(std::span<const uint8_t> header, const Block& set_a, const Block* set_b, int set_b_slot,
               const Layout& layout)
    : set_a_(set_a), set_b_(set_b), set_b_slot_(set_b_slot), layout_(layout)
{
    if (header.size() < kHeaderChannelGroups + 2)
    {
        throw std::invalid_argument("the sound data's header is too short");
    }

    // The driver's start-up (0x037FABC0): the players' tracks, the channel groups and the reverb.
    for (int p = 0; p < kPlayers; ++p)
    {
        players_[p].max_tracks = kMaxTracks[p];
        players_[p].tracks.resize(kMaxTracks[p]);
    }
    channels_.resize(kChannels);
    const uint16_t groups = xsflib::LoadLe16(&header[kHeaderChannelGroups]);
    SetChannelGroups(static_cast<uint32_t>(MessageKind::kChannelGroups) | (groups & 7u) << 8 |
                     ((groups >> 3) & 3u) << 16);
    SetReverb(static_cast<uint32_t>(header[kHeaderReverb]) << 16);
    writes_.clear();
}

void Player::Post(uint32_t message)
{
    messages_.push_back(message);
}

void Player::SetBranchFlag(int value)
{
    branch_flag_ = value & 0xFF;
}

void Player::SetPitchMode(int mode)
{
    pitch_mode_ = mode & 0xFF;
}

const std::vector<LoopMark>& Player::LoopMarks() const
{
    return loop_marks_;
}

const std::vector<Channel>& Player::Channels() const
{
    return channels_;
}

const std::array<PlayerState, kPlayers>& Player::Players() const
{
    return players_;
}

int Player::Reverb() const
{
    return reverb_;
}

bool Player::Busy() const
{
    const auto playing = [](const PlayerState& player)
    {
        return (player.state & 0xF) != 0;
    };
    const auto sounding = [](const Channel& channel)
    {
        return channel.state != 0;
    };
    return std::any_of(players_.begin(), players_.end(), playing) ||
           std::any_of(channels_.begin(), channels_.end(), sounding);
}

std::vector<RegisterWrite> Player::Frame()
{
    writes_.clear();
    loop_marks_.clear();
    for (Channel& channel : channels_)
    {
        channel.started = false;
    }

    // The messages (0x037FA6DC): up to 64, until the first word whose kind is out of range.
    const std::size_t count = std::min<std::size_t>(messages_.size(), kMaxMessages);
    for (std::size_t i = 0; i < count; ++i)
    {
        const uint32_t kind = messages_[i] & 0xFF;
        if (kind < 1 || kind > 16)
        {
            break;
        }
        Dispatch(messages_[i]);
    }
    messages_.clear();

    // The players' fades and tracks, in order.
    for (int p = 0; p < kPlayers; ++p)
    {
        Fade(p);
        if ((players_[p].state & 0xF) == 1)
        {
            Step(p);
        }
    }

    // The channels: an update for each that plays, and cleared registers for each that's free.
    for (std::size_t i = 0; i < active_end_; ++i)
    {
        if (channels_[i].state != 0)
        {
            Update(channels_[i]);
        }
        else
        {
            ClearChannel(channels_[i]);
        }
    }

    // The pitch modes 2 and 3 (0x037FA9E0).
    if (pitch_mode_ == 2)
    {
        pitch_offset_ = kWobble[counter_ & 7];
        counter_ = counter_ + 1 > 96 ? 0 : counter_ + 1;
    }
    else if (pitch_mode_ == 3)
    {
        pitch_offset_ = (counter_ & 1) != 0 ? 24 : -24;
        counter_ = counter_ + 1 > 96 ? 0 : counter_ + 1;
    }
    else
    {
        pitch_offset_ = 0;
    }

    return writes_;
}

void Player::Dispatch(uint32_t message)
{
    switch (static_cast<MessageKind>(message & 0xFF))
    {
    case MessageKind::kStart:
        Start(message);
        break;
    case MessageKind::kStop:
        Stop(message);
        break;
    case MessageKind::kPause:
        Pause(message);
        break;
    case MessageKind::kResume:
        Resume(message);
        break;
    case MessageKind::kRestartPlayers:
        ForPlayers(message, &Player::Start);
        break;
    case MessageKind::kStopPlayers:
        ForPlayers(message, &Player::Stop);
        break;
    case MessageKind::kPausePlayers:
        ForPlayers(message, &Player::Pause);
        break;
    case MessageKind::kResumePlayers:
        ForPlayers(message, &Player::Resume);
        break;
    case MessageKind::kLevel:
        SetLevel(message);
        break;
    case MessageKind::kVolume:
    case MessageKind::kPitch:
    case MessageKind::kTempo:
    case MessageKind::kPan:
        {
            // 0x037F86D4, 0x037F8680, 0x037F862C and 0x037F85D8: a value for each player of the mask.
            const MessageKind kind = static_cast<MessageKind>(message & 0xFF);
            const uint8_t value = static_cast<uint8_t>(message >> 8);
            uint32_t mask = message >> 16;
            for (PlayerState& player : players_)
            {
                if ((mask & 1) != 0)
                {
                    if (kind == MessageKind::kVolume)
                    {
                        player.volume = value;
                    }
                    else if (kind == MessageKind::kPitch)
                    {
                        player.pitch = static_cast<int8_t>(value);
                    }
                    else if (kind == MessageKind::kTempo)
                    {
                        player.tempo = static_cast<int8_t>(value);
                    }
                    else
                    {
                        player.pan = static_cast<int8_t>(value);
                    }
                }
                mask >>= 1;
            }
            break;
        }
    case MessageKind::kChannelGroups:
        SetChannelGroups(message);
        break;
    case MessageKind::kReverb:
        SetReverb(message);
        break;
    default:
        break;
    }
}

uint32_t Player::SetAddress(bool b) const
{
    return b ? layout_.set_b : layout_.set_a;
}

uint32_t Player::SoundEntry(int set, int sound) const
{
    const uint32_t block = SetAddress(set != 0);

    return block + Read32(block + 0xC) + static_cast<uint32_t>(sound - 1) * 8;
}

uint8_t Player::Read8(uint32_t address) const
{
    const auto read = [address](const Block* block, uint32_t base) -> int
    {
        if (block == nullptr || address < base || address - base >= block->Bytes().size())
        {
            return -1;
        }
        return block->Bytes()[address - base];
    };

    int value = read(&set_a_, layout_.set_a);
    if (value < 0)
    {
        if (set_b_ == nullptr && address >= layout_.set_b && address < layout_.area + kCaptureLeft)
        {
            throw NoSetB();
        }
        value = read(set_b_, layout_.set_b);
    }
    return value < 0 ? 0 : static_cast<uint8_t>(value);
}

uint16_t Player::Read16(uint32_t address) const
{
    return static_cast<uint16_t>(Read8(address) | Read8(address + 1) << 8);
}

uint32_t Player::Read32(uint32_t address) const
{
    return static_cast<uint32_t>(Read16(address)) | static_cast<uint32_t>(Read16(address + 2)) << 16;
}

void Player::Write(uint32_t address, int size, uint32_t value)
{
    const uint32_t mask = size == 4 ? 0xFFFFFFFF : (1u << (size * 8)) - 1;
    writes_.push_back({address, static_cast<uint8_t>(size), value & mask});
}

// 0x037F8D14.
void Player::Start(uint32_t message)
{
    const uint8_t fade = static_cast<uint8_t>(message >> 8);
    const int set = static_cast<int>(Bits(message, 16, 6));
    int sound = static_cast<int>(Bits(message, 22, 10));

    // Sound 30 of set A plays one of four sounds by a counter.
    if (set == 0 && sound == 30)
    {
        sound += PitchTable()[counter_] & 3;
        counter_ = counter_ + 1 > 96 ? 0 : counter_ + 1;
    }

    const uint32_t block = SetAddress(set != 0);
    const uint32_t table = block + Read32(block + 0xC);
    const uint32_t entry = table + static_cast<uint32_t>(sound - 1) * 8;
    const uint32_t flags = Read32(entry + 4);
    const uint32_t header = block + Read32(entry);

    // Sounds 151 to 158 of slot 9 take the players from their flags' on, in turn.
    int p = static_cast<int>(flags & 0xF);
    if (set == 9 && sound > 150 && sound < 159)
    {
        cycle_ = cycle_ + 1 > 2 ? 0 : cycle_ + 1;
        p += cycle_;
    }
    if (p >= kPlayers)
    {
        throw std::runtime_error("a sound names a player past the 16th");
    }

    PlayerState& player = players_[p];
    if (sound == 0 || Read32(entry) == 0 || static_cast<uint32_t>(sound) > Read32(block + 0x10))
    {
        return;
    }

    // The priority: a sound with the delay field 63 takes a busy player only with a higher priority.
    const uint32_t delay = Bits(flags, 14, 6);
    const uint32_t priority = Bits(flags, 11, 3);
    const uint32_t state = player.state & 0xF;
    if (state != 0 && (delay == 63 ? player.priority >= priority : player.priority > priority))
    {
        return;
    }

    // The sound that the player played ends. The driver reads its flags from the new sound's table.
    if (state == 1 || state == 2)
    {
        player.state = 0;
        RunAction(p, Read32(table + static_cast<uint32_t>(player.sound - 1) * 8 + 4), true);
    }
    FreeChannels(p);

    for (Track& track : player.tracks)
    {
        track = {};
    }
    const uint32_t count = std::min<uint32_t>(Read32(header), player.max_tracks);
    for (uint32_t i = 0; i < count; ++i)
    {
        Track& track = player.tracks[i];
        track.position = Read32(header + 4 + i * 4) + block;
        track.pan = 63;
        track.volume = 127;
    }

    player.sound = 0;
    player.delay = 0;
    player.next_length = 0;
    player.state = 0;
    player.volume = 0;
    player.pitch = 0;
    player.tempo = 0;
    player.fade_length = 0;
    player.fade_count = 0;
    player.level_length = 0;
    player.level_count = 0;
    player.fade = 0;
    player.level = 0;
    player.target = 0;
    player.saved = 0;

    player.priority = static_cast<uint8_t>(priority);
    player.sound = static_cast<uint16_t>(sound);
    player.state = 1;
    player.target = static_cast<uint8_t>(Bits(flags, 4, 7));
    player.delay = static_cast<uint8_t>(delay * 2);
    player.fade = 2;
    player.fade_length = fade;
    player.fade_count = static_cast<uint8_t>(Divide(player.volume * player.fade_length, player.target));
    player.set = static_cast<uint8_t>(set);
    player.set_b = set != 0;
    player.pan = 0;
    if (player.delay == 126)
    {
        player.delay = 0;
    }
    if (p == 0)
    {
        pitch_mode_ = 0;
    }

    RunAction(p, flags, false);
}

// 0x037F8BB4.
void Player::Stop(uint32_t message)
{
    const uint8_t fade = static_cast<uint8_t>(message >> 8);
    const int set = static_cast<int>(Bits(message, 16, 6));
    const int sound = static_cast<int>(Bits(message, 22, 10));
    const uint32_t flags = Read32(SoundEntry(set, sound) + 4);

    // Sounds 151 to 158 of slot 9 stop players 4 to 6 at once.
    if (set == 9 && sound > 150 && sound < 159)
    {
        for (int p = 4; p < 7; ++p)
        {
            PlayerState& player = players_[p];
            player.level = 0;
            player.level_length = 0;
            player.level_count = 0;
            player.fade = 1;
            player.fade_length = 0;
            player.fade_count = 0;
        }
        return;
    }

    const int p = static_cast<int>(flags & 0xF);
    PlayerState& player = players_[p];
    if ((player.state & 0xF) == 0 || player.set != set || player.sound != sound)
    {
        return;
    }

    player.level = 0;
    player.level_length = 0;
    player.level_count = 0;
    player.fade = 1;
    player.fade_length = fade;
    player.fade_count = static_cast<uint8_t>(fade + Divide(-player.volume * fade, player.target));
    RunAction(p, flags, true);
}

// 0x037F8AA8: a fade-out that pauses the sound.
void Player::Pause(uint32_t message)
{
    const uint8_t fade = static_cast<uint8_t>(message >> 8);
    const int set = static_cast<int>(Bits(message, 16, 6));
    const int sound = static_cast<int>(Bits(message, 22, 10));
    const uint32_t flags = Read32(SoundEntry(set, sound) + 4);
    PlayerState& player = players_[flags & 0xF];
    if ((player.state & 0xF) != 1 || player.set != set || player.sound != sound || player.level == 1)
    {
        return;
    }

    if (player.level == 0)
    {
        player.saved = player.volume;
    }
    player.level = 1;
    player.level_length = fade;
    player.level_count = static_cast<uint8_t>(fade + Divide(-player.volume * fade, player.saved));
}

// 0x037F89BC: a fade-in that resumes the sound.
void Player::Resume(uint32_t message)
{
    const uint8_t fade = static_cast<uint8_t>(message >> 8);
    const int set = static_cast<int>(Bits(message, 16, 6));
    const int sound = static_cast<int>(Bits(message, 22, 10));
    const uint32_t flags = Read32(SoundEntry(set, sound) + 4);
    PlayerState& player = players_[flags & 0xF];
    if (player.set != set || player.sound != sound || player.level == 2 || player.level == 0)
    {
        return;
    }

    player.state = 1;
    player.level = 2;
    player.level_length = fade;
    player.level_count = static_cast<uint8_t>(Divide(player.volume * player.level_length, player.saved));
}

// 0x037F8944, 0x037F88CC, 0x037F8854 and 0x037F87DC: a message for the sound of each player of the mask.
void Player::ForPlayers(uint32_t message, void (Player::*action)(uint32_t))
{
    const uint32_t fade = (message >> 8) & 0xFF;
    uint32_t mask = message >> 16;
    for (const PlayerState& player : players_)
    {
        if ((mask & 1) != 0)
        {
            (this->*action)(static_cast<uint32_t>(player.sound) << 22 | static_cast<uint32_t>(player.set) << 16 |
                            fade << 8);
        }
        mask >>= 1;
    }
}

// 0x037F8720: a level in percent for each player of the mask.
void Player::SetLevel(uint32_t message)
{
    const uint8_t level = static_cast<uint8_t>(Divide(static_cast<int32_t>((message >> 8) & 0xFF) * 127, 100));
    uint32_t mask = message >> 16;
    for (PlayerState& player : players_)
    {
        if ((mask & 1) != 0)
        {
            if (player.level == 0)
            {
                player.saved = player.volume;
            }
            else if (player.level == 3)
            {
                player.next_length = player.level_length;
                player.saved = player.volume;
            }
            player.level = 3;
            player.level_length = level;
        }
        mask >>= 1;
    }
}

// 0x037FA7E0: the squares from channel 8 on, the noise from channel 14 on, and the samples on the rest.
void Player::SetChannelGroups(uint32_t message)
{
    const int squares = static_cast<int>((message >> 8) & 0xFF);
    const int noise = static_cast<int>((message >> 16) & 0xFF);
    std::vector<int> order;
    for (int c = 8; c < squares + 8; ++c)
    {
        order.push_back(c);
    }
    for (int c = 14; c < noise + 14; ++c)
    {
        order.push_back(c);
    }
    for (const int c : {0, 2, 4, 5, 6, 7})
    {
        order.push_back(c);
    }
    for (int c = squares + 8; c < 14; ++c)
    {
        order.push_back(c);
    }
    for (int c = noise + 14; c < 16; ++c)
    {
        order.push_back(c);
    }
    order.push_back(kReverbLeft);
    order.push_back(kReverbRight);
    if (order.size() != kChannels)
    {
        throw std::runtime_error("the channel groups take more than 16 channels");
    }

    for (std::size_t i = 0; i < order.size(); ++i)
    {
        channels_[i].hardware = static_cast<uint8_t>(order[i]);
        channels_[i].state = 0;
    }
    squares_end_ = static_cast<std::size_t>(squares);
    noise_end_ = static_cast<std::size_t>(squares + noise);
    if (reverb_ == 0)
    {
        active_end_ = kChannels;
    }
}

// 0x037F83F4: the capture reverb on channels 1 and 3.
void Player::SetReverb(uint32_t message)
{
    const int level = static_cast<int>((message >> 16) & 0xFF);
    if (reverb_ != 0 && level == 0)
    {
        active_end_ = kChannels;
        channels_[kChannels - 2].state = 0;
        channels_[kChannels - 1].state = 0;
        Write(kCaptureControl, 2, 0);
    }
    if (level != 0)
    {
        if (reverb_ == 0)
        {
            const uint32_t left = layout_.area + kCaptureLeft;
            const uint32_t right = layout_.area + kCaptureRight;
            active_end_ = kChannels - 2;
            ClearChannel(channels_[kChannels - 2]);
            ClearChannel(channels_[kChannels - 1]);
            const uint32_t left_registers = kChannelRegisters + kReverbLeft * 16;
            const uint32_t right_registers = kChannelRegisters + kReverbRight * 16;
            Write(kCapture0Address, 4, left);
            Write(kCapture1Address, 4, right);
            Write(kCapture0Length, 2, kReverbLength);
            Write(kCapture1Length, 2, kReverbLength);
            Write(left_registers + kSad, 4, left);
            Write(right_registers + kSad, 4, right);
            Write(left_registers + kLen, 4, kReverbLength);
            Write(right_registers + kLen, 4, kReverbLength);
            Write(left_registers + kPnt, 2, 0);
            Write(right_registers + kPnt, 2, 0);
            Write(left_registers + kTmr, 2, kReverbTimer);
            Write(right_registers + kTmr, 2, kReverbTimer);
            Write(left_registers + kPanByte, 1, 0);
            Write(right_registers + kPanByte, 1, 127);
            Write(left_registers + kStartByte, 1, kReverbControl);
            Write(right_registers + kStartByte, 1, kReverbControl);
            Write(kCaptureControl, 2, kCaptureStart);
        }
        Write(kChannelRegisters + kReverbLeft * 16 + kCnt, 2, static_cast<uint32_t>(level));
        Write(kChannelRegisters + kReverbRight * 16 + kCnt, 2, static_cast<uint32_t>(level));
    }
    reverb_ = level;
}

// 0x037FA5E0: the action that a sound's flags name at its start or its end.
void Player::RunAction(int p, uint32_t flags, bool end)
{
    const int index = static_cast<int>(end ? flags >> 26 : Bits(flags, 20, 6)) - 1;
    if (index < 0)
    {
        return;
    }

    const PlayerState& player = players_[p];
    const uint32_t message = Read32(SetAddress(player.set_b) + kEndActionsOffset + static_cast<uint32_t>(index) * 4);
    const uint32_t kind = message & 0xFF;
    if (kind == 0 || kind > 16)
    {
        return;
    }

    // A message about a sound of set B applies only to the block that's loaded.
    if (kind >= 2 && kind <= 5)
    {
        const uint32_t set = Bits(message, 16, 6);
        if (set != 0 && static_cast<int>(set) != set_b_slot_)
        {
            return;
        }
    }
    Dispatch(message);
}

} // namespace ntdlib::spd
