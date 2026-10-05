// Wire format between the plugin-scan worker process and its parent: a job file in, a frame file out.
#pragma once

#include "Juce.h"

#include <vector>

namespace hr::scan
{
// `headroom --scan-worker <jobFile> <outFile> <parentPid>`. The worker reads the jobs, runs them one by one on its
// message thread and appends one frame per line to outFile. The parent polls that file: a plain file (not a pipe)
// means nothing can block, a hung plugin can be killed from outside, and whatever the plugin prints to stdout is
// irrelevant.
inline constexpr const char* kWorkerFlag = "--scan-worker";

// Job file: first line "HRJOBS 1 [testhooks]", then one job per line, fields separated by TAB.
//   S <format> <recursive 0|1> <searchPath ';'-joined>     list the candidate files / URIs of a format
//   P <format> <fileOrIdentifier>                           load one file and describe the plugins in it
struct WireJob
{
    enum class Kind { search, scan };

    Kind kind = Kind::scan;
    juce::String format;
    juce::String argument;          // scan: the file or identifier; search: FileSearchPath::toString()
    bool recursive = true;          // search only
};

// Frames the worker appends to the out file (one per line, complete lines only):
//   H <pid>                     worker is up (before it builds the plugin formats)
//   B <jobIndex>                about to run job <jobIndex>
//   R <jobIndex> <xml>          result of a scan job: <LIST> of PLUGIN elements (empty list = nothing usable in the file)
//   F <jobIndex> <xml>          result of a search job: <FILES><FILE id="..."/></FILES>
//   D                           every job ran; the worker exits right after
// A worker that dies (or is killed) leaves a "B" without its "R"/"F": that job is the culprit.

// TAB / CR / LF in a field would break the line format. Real paths and LV2 URIs never contain them.
inline bool isWireSafe (const juce::String& s)
{
    return ! s.containsAnyOf ("\t\r\n");
}

// Splits at the first (maxFields - 1) TABs; the last field keeps the rest of the line (including any TABs). Unlike
// StringArray::fromTokens this keeps empty fields, so an empty search path can't shift the columns.
inline juce::StringArray splitFields (const juce::String& line, int maxFields)
{
    juce::StringArray fields;
    auto rest = line;

    while (fields.size() < maxFields - 1)
    {
        const auto tab = rest.indexOfChar ('\t');

        if (tab < 0)
            break;

        fields.add (rest.substring (0, tab));
        rest = rest.substring (tab + 1);
    }

    fields.add (rest);
    return fields;
}

inline juce::String encodeJobs (const std::vector<WireJob>& jobs, bool testHooks)
{
    juce::String text ("HRJOBS 1");

    if (testHooks)
        text << " testhooks";

    text << "\n";

    for (const auto& job : jobs)
    {
        if (job.kind == WireJob::Kind::search)
            text << "S\t" << job.format << "\t" << (job.recursive ? "1" : "0") << "\t" << job.argument << "\n";
        else
            text << "P\t" << job.format << "\t" << job.argument << "\n";
    }

    return text;
}

// Returns false if the header is missing or from another protocol version. Malformed job lines are skipped, but
// keep their position (as a scan of nothing) so frame indices still line up with the parent's list.
inline bool decodeJobs (const juce::String& text, std::vector<WireJob>& jobs, bool& testHooks)
{
    jobs.clear();
    testHooks = false;

    auto lines = juce::StringArray::fromLines (text);

    if (lines.isEmpty() || ! lines[0].startsWith ("HRJOBS 1"))
        return false;

    testHooks = lines[0].containsWholeWord ("testhooks");
    lines.remove (0);

    for (const auto& line : lines)
    {
        if (line.isEmpty())
            continue;

        WireJob job;

        if (line.startsWith ("S\t"))
        {
            const auto fields = splitFields (line, 4);

            if (fields.size() == 4)
            {
                job.kind = WireJob::Kind::search;
                job.format = fields[1];
                job.recursive = fields[2] == "1";
                job.argument = fields[3];
            }
        }
        else if (line.startsWith ("P\t"))
        {
            const auto fields = splitFields (line, 3);

            if (fields.size() == 3)
            {
                job.kind = WireJob::Kind::scan;
                job.format = fields[1];
                job.argument = fields[2];
            }
        }

        jobs.push_back (std::move (job));
    }

    return true;
}

inline juce::String singleLineXml (const juce::XmlElement& xml)
{
    return xml.toString (juce::XmlElement::TextFormat().withoutHeader().singleLine());
}
}
