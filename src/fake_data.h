#pragma once
#include "types.h"
#include <vector>
#include <string>

struct FakeChunk {
    BlockType type;
    std::string text;
    bool isMarkdown;

    FakeChunk(BlockType t, const std::string& txt, bool md = false)
        : type(t), text(txt), isMarkdown(md) {}
};

std::vector<FakeChunk> GetFakeConversation();
