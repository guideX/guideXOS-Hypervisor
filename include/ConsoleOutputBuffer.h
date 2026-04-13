#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <cstddef>
#include <cstdint>

namespace ia64 {

/**
 * ConsoleOutputBuffer - Thread-safe circular buffer for console output
 * 
 * Stores console output with configurable scrollback limit. Provides
 * efficient line-based storage with real-time access to recent output.
 * 

 * Features:
 * - Thread-safe read/write operations
 * - Configurable maximum line count (scrollback buffer)
 * - Automatic overflow handling (FIFO removal)
 * - Line-based and character-based append
 * - Range-based retrieval
 * - Total output statistics
 * 
 * Usage:
 * ```
 * ConsoleOutputBuffer buffer(1000); // 1000 lines scrollback
 * 
 * buffer.appendChar('H');
 * buffer.appendChar('i');
 * buffer.appendChar('\n');
 * 
 * buffer.appendLine("Hello, world!");
 * 
 * std::vector<std::string> lines = buffer.getLines(0, 10);
 * std::string recent = buffer.getRecentOutput(256);
 * ```
 */
class ConsoleOutputBuffer {
public:
    /**
     * Constructor
     * 
     * @param maxLines Maximum number of lines to keep in buffer (0 = unlimited)
     */
    explicit ConsoleOutputBuffer(size_t maxLines = 10000);
    
    /**
     * Destructor
     */
    ~ConsoleOutputBuffer();
    
    /**
     * Append a single character to the buffer
     * 
     * If character is newline, completes current line and adds to buffer.
     * 
     * @param c Character to append
     */
    void appendChar(char c);
    
    /**
     * Append a complete line to the buffer
     * 
     * @param line Line to append (newline will be added automatically)
     */
    void appendLine(const std::string& line);
    
    /**
     * Append raw text to the buffer
     * 
     * Text is processed character by character and split into lines.
     * 
     * @param text Text to append
     */
    void appendText(const std::string& text);
    
    /**
     * Get all lines in the buffer
     * 
     * @return Vector of all lines
     */
    std::vector<std::string> getAllLines() const;
    
    /**
     * Get a range of lines from the buffer
     * 
     * @param startLine Start line index (0-based)
     * @param count Number of lines to retrieve (0 = all remaining)
     * @return Vector of lines
     */
    std::vector<std::string> getLines(size_t startLine, size_t count = 0) const;
    
    /**
     * Get the most recent N bytes of output
     * 
     * Returns recent output, up to maxBytes. May return less if buffer
     * contains less data.
     * 
     * @param maxBytes Maximum bytes to retrieve
     * @return Recent output string
     */
    std::string getRecentOutput(size_t maxBytes) const;
    
    /**
     * Get output since a specific line number
     * 
     * @param sinceLineNumber Line number (0-based)
     * @return Vector of lines since the specified line
     */
    std::vector<std::string> getLinesSince(size_t sinceLineNumber) const;
    
    /**
     * Get the current line count
     * 
     * @return Number of complete lines in buffer
     */
    size_t getLineCount() const;
    
    /**
     * Get the total number of bytes written (including overflow)
     * 
     * @return Total bytes written to buffer (all time)
     */
    uint64_t getTotalBytesWritten() const;
    
    /**
     * Get the current buffer size in bytes
     * 
     * @return Current buffer size (excluding overflowed data)
     */
    size_t getCurrentBufferSize() const;
    
    /**
     * Clear all buffered output
     */
    void clear();
    
    /**
     * Get the maximum number of lines
     * 
     * @return Maximum line count (0 = unlimited)
     */
    size_t getMaxLines() const { return maxLines_; }
    
    /**
     * Set the maximum number of lines
     * 
     * If new limit is smaller than current line count, oldest lines
     * will be removed.
     * 
     * @param maxLines Maximum line count (0 = unlimited)
     */
    void setMaxLines(size_t maxLines);
    
private:
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
    std::string currentLine_;
    size_t maxLines_;
    uint64_t totalBytesWritten_;
    
    void completeLine();
    void enforceMaxLines();
};

} // namespace ia64
