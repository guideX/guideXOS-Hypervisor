#include "ConsoleOutputBuffer.h"
#include <algorithm>
#include <vector>

namespace ia64 {

ConsoleOutputBuffer::ConsoleOutputBuffer(size_t maxLines)
    : mutex_()
    , lines_()
    , currentLine_()
    , maxLines_(maxLines)
    , totalBytesWritten_(0) {

}

ConsoleOutputBuffer::~ConsoleOutputBuffer() {
}

void ConsoleOutputBuffer::appendChar(char c) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    currentLine_.push_back(c);
    totalBytesWritten_++;
    
    if (c == '\n') {
        completeLine();
    }
}

void ConsoleOutputBuffer::appendLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!currentLine_.empty()) {
        completeLine();
    }
    
    lines_.push_back(line);
    totalBytesWritten_ += line.length() + 1; // +1 for implicit newline
    enforceMaxLines();
}

void ConsoleOutputBuffer::appendText(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (char c : text) {
        currentLine_.push_back(c);
        totalBytesWritten_++;
        
        if (c == '\n') {
            completeLine();
        }
    }
}

std::vector<std::string> ConsoleOutputBuffer::getAllLines() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::string> result(lines_.begin(), lines_.end());
    
    if (!currentLine_.empty()) {
        result.push_back(currentLine_);
    }
    
    return result;
}

std::vector<std::string> ConsoleOutputBuffer::getLines(size_t startLine, size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::string> result;
    
    size_t totalLines = lines_.size();
    if (!currentLine_.empty()) {
        totalLines++;
    }
    
    if (startLine >= totalLines) {
        return result;
    }
    
    size_t endLine = totalLines;
    if (count > 0) {
        endLine = std::min(startLine + count, totalLines);
    }
    
    for (size_t i = startLine; i < endLine; ++i) {
        if (i < lines_.size()) {
            result.push_back(lines_[i]);
        } else if (i == lines_.size() && !currentLine_.empty()) {
            result.push_back(currentLine_);
        }
    }
    
    return result;
}

std::string ConsoleOutputBuffer::getRecentOutput(size_t maxBytes) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string result;
    size_t bytesCollected = 0;
    
    if (!currentLine_.empty() && bytesCollected < maxBytes) {
        size_t bytesToAdd = std::min(currentLine_.length(), maxBytes - bytesCollected);
        if (bytesToAdd < currentLine_.length()) {
            result = currentLine_.substr(currentLine_.length() - bytesToAdd);
        } else {
            result = currentLine_;
        }
        bytesCollected += bytesToAdd;
    }
    
    if (bytesCollected < maxBytes && !lines_.empty()) {
        std::string tempResult;
        
        for (auto it = lines_.rbegin(); it != lines_.rend() && bytesCollected < maxBytes; ++it) {
            size_t lineLength = it->length() + 1; // +1 for newline
            
            if (bytesCollected + lineLength <= maxBytes) {
                tempResult = *it + "\n" + tempResult;
                bytesCollected += lineLength;
            } else {
                size_t remainingBytes = maxBytes - bytesCollected;
                if (remainingBytes > 0) {
                    size_t startPos = it->length() - (remainingBytes - 1);
                    tempResult = it->substr(startPos) + "\n" + tempResult;
                    bytesCollected += remainingBytes;
                }
                break;
            }
        }
        
        result = tempResult + result;
    }
    
    return result;
}

std::vector<std::string> ConsoleOutputBuffer::getLinesSince(size_t sinceLineNumber) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::string> result;
    
    size_t totalLines = lines_.size();
    if (!currentLine_.empty()) {
        totalLines++;
    }
    
    if (sinceLineNumber >= totalLines) {
        return result;
    }
    
    for (size_t i = sinceLineNumber; i < totalLines; ++i) {
        if (i < lines_.size()) {
            result.push_back(lines_[i]);
        } else if (i == lines_.size() && !currentLine_.empty()) {
            result.push_back(currentLine_);
        }
    }
    
    return result;
}

size_t ConsoleOutputBuffer::getLineCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_.size();
}

uint64_t ConsoleOutputBuffer::getTotalBytesWritten() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalBytesWritten_;
}

size_t ConsoleOutputBuffer::getCurrentBufferSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    size_t size = currentLine_.length();
    for (const auto& line : lines_) {
        size += line.length() + 1; // +1 for newline
    }
    
    return size;
}

void ConsoleOutputBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    lines_.clear();
    currentLine_.clear();
    totalBytesWritten_ = 0;
}

void ConsoleOutputBuffer::setMaxLines(size_t maxLines) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    maxLines_ = maxLines;
    enforceMaxLines();
}

void ConsoleOutputBuffer::completeLine() {
    if (currentLine_.empty() || currentLine_.back() != '\n') {
        return;
    }
    
    std::string line = currentLine_;
    if (line.back() == '\n') {
        line.pop_back();
    }
    
    lines_.push_back(line);
    currentLine_.clear();
    enforceMaxLines();
}

void ConsoleOutputBuffer::enforceMaxLines() {
    if (maxLines_ == 0) {
        return;
    }
    
    while (lines_.size() > maxLines_) {
        lines_.pop_front();
    }
}

} // namespace ia64
