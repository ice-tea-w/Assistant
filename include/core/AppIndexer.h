#pragma once

#include <QString>
#include <QList>

/// Windows Start Menu app launcher indexer.
///
/// Scans the user + all-users Start Menu directories for `.lnk` shortcuts,
/// resolves each to its target path via the Shell Link COM interface, and
/// returns a flat, de-duplicated, name-sorted list of launchable apps.
namespace AppIndexer
{
    /// A single resolved application entry.
    struct Entry
    {
        QString name;   // display name (shortcut filename without ".lnk")
        QString path;   // resolved target (e.g. "C:\\Program Files\\App\\app.exe")
    };

    /// Scan all Start Menu folders and return resolved entries.
    /// Entries whose target cannot be resolved are silently skipped.
    /// Never throws — logs progress via Logger.
    QList<Entry> scan();
}
