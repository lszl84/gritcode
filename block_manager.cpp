#include "block_manager.h"
#include <wx/window.h>

BlockManager::BlockManager() 
    : totalLines(0) {
    // Default font settings - will be updated when control is created
    normalFont = wxFont(12, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    thinkingFont = wxFont(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_ITALIC, wxFONTWEIGHT_LIGHT);
    codeFont = wxFont(11, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
}

BlockManager::~BlockManager() = default;

void BlockManager::AppendStream(BlockType type, const wxString& text, bool rtl) {
    if (blocks.empty() || blocks.back()->type != type || blocks.back()->rightToLeft != rtl) {
        // Create new block
        auto block = std::make_unique<TextBlock>(type, text, rtl);
        block->lineCount = CalculateBlockLines(*block);
        blocks.push_back(std::move(block));
    } else {
        // Append to existing block of same type and direction
        blocks.back()->text += text;
        blocks.back()->lineCount = CalculateBlockLines(*blocks.back());
    }
    
    RecalculateLineCounts();
}

const TextBlock* BlockManager::GetBlockForLine(int line) const {
    if (line < 0 || line >= totalLines || blocks.empty()) {
        return nullptr;
    }
    
    int currentLine = 0;
    for (const auto& block : blocks) {
        if (line < currentLine + block->lineCount) {
            return block.get();
        }
        currentLine += block->lineCount;
    }
    
    return nullptr;
}

const TextBlock* BlockManager::GetBlock(size_t index) const {
    if (index >= blocks.size()) return nullptr;
    return blocks[index].get();
}

int BlockManager::GetLineInBlock(int virtualLine, size_t& blockIndex) const {
    if (virtualLine < 0 || virtualLine >= totalLines || blocks.empty()) {
        blockIndex = 0;
        return 0;
    }
    
    int currentLine = 0;
    for (size_t i = 0; i < blocks.size(); i++) {
        if (virtualLine < currentLine + blocks[i]->lineCount) {
            blockIndex = i;
            return virtualLine - currentLine;
        }
        currentLine += blocks[i]->lineCount;
    }
    
    blockIndex = blocks.size() - 1;
    return 0;
}

void BlockManager::Clear() {
    blocks.clear();
    totalLines = 0;
}

void BlockManager::SetBlockLoading(size_t blockIndex, bool loading) {
    if (blockIndex < blocks.size()) {
        blocks[blockIndex]->isLoading = loading;
    }
}

TextBlock* BlockManager::GetLastBlock() {
    if (blocks.empty()) return nullptr;
    return blocks.back().get();
}

void BlockManager::RecalculateLineCounts() {
    totalLines = 0;
    for (const auto& block : blocks) {
        totalLines += block->lineCount;
    }
}

int BlockManager::CalculateBlockLines(const TextBlock& block) {
    // Simple line counting - in production, this would use actual text wrapping
    // For now, assume each newline is a line break
    int lines = 1;
    for (size_t i = 0; i < block.text.length(); i++) {
        if (block.text[i] == '\n') {
            lines++;
        }
    }
    
    // For code blocks, we might want different line heights
    if (block.type == BlockType::CODE) {
        return lines;
    }
    
    return lines;
}
