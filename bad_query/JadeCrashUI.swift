//
//  JadeCrashUI.swift
//  Jade
//
//  Boot hook + SwiftUI front-end for JadeCrash.c.
//  Logs live in <app container>/Documents/JadeCrashLogs/ and are exposed in
//  Settings → Diagnostics → Crash Logs, with copy/share so they can be sent
//  off the device without a Mac or a cable.
//

import Foundation
import SwiftUI
import UIKit

// MARK: - Boot

enum JadeCrashBoot {
    /// Installs the signal + exception handlers. Call once, as early as possible.
    static func start() {
        let info = Bundle.main.infoDictionary ?? [:]
        let short = info["CFBundleShortVersionString"] as? String ?? "?"
        let build = info["CFBundleVersion"] as? String ?? "?"
        let bundleID = Bundle.main.bundleIdentifier ?? "unknown"
        let os = ProcessInfo.processInfo.operatingSystemVersionString

        JadeCrashSetContext("\(short) (\(build))", bundleID, os, nil)
        JadeCrashInstall()
        breadcrumb("app: launched, bundle=\(bundleID) version=\(short) (\(build))")
    }

    /// Free-form breadcrumb; cheap, appends one line to the session log.
    static func breadcrumb(_ message: String) {
        JadeCrashBreadcrumbString(message)
    }

    static var directory: URL? {
        guard let cPath = JadeCrashDirectory() else { return nil }
        return URL(fileURLWithPath: String(cString: cPath), isDirectory: true)
    }

    static var sessionLogURL: URL? {
        guard let cPath = JadeCrashSessionLogPath() else { return nil }
        return URL(fileURLWithPath: String(cString: cPath))
    }
}

// MARK: - Scene phase tracking

/// Zero-size probe view: drops the "session active" marker whenever the app
/// backgrounds cleanly, and restores it on return. Without this, every launch
/// would look like the previous run crashed.
struct JadeScenePhaseObserver: View {
    @Environment(\.scenePhase) private var scenePhase

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .allowsHitTesting(false)
            .onChange(of: scenePhase) { _, phase in
                switch phase {
                case .active:
                    JadeCrashMarkSessionActive()
                    JadeCrashBoot.breadcrumb("scene: active")
                case .inactive:
                    JadeCrashBoot.breadcrumb("scene: inactive")
                case .background:
                    JadeCrashBoot.breadcrumb("scene: background")
                    JadeCrashMarkCleanExit()
                @unknown default:
                    break
                }
            }
    }
}

// MARK: - Model

struct JadeCrashLogEntry: Identifiable, Hashable {
    let url: URL
    let modified: Date
    let size: Int

    var id: String { url.path }
    var name: String { url.lastPathComponent }

    var kind: String {
        if name.hasPrefix("crash-") { return "Crash" }
        if name.hasPrefix("exception-") { return "Exception" }
        if name.hasPrefix("session-") { return "Session" }
        return "Report"
    }

    var symbolName: String {
        switch kind {
        case "Crash": return "bolt.trianglebadge.exclamationmark"
        case "Exception": return "exclamationmark.octagon"
        case "Session": return "list.bullet.rectangle"
        default: return "doc.text"
        }
    }

    var sizeText: String {
        ByteCountFormatter.string(fromByteCount: Int64(size), countStyle: .file)
    }

    var text: String {
        (try? String(contentsOf: url, encoding: .utf8)) ?? "(unreadable report)"
    }

    static func loadAll() -> [JadeCrashLogEntry] {
        guard let dir = JadeCrashBoot.directory else { return [] }
        let keys: [URLResourceKey] = [.contentModificationDateKey, .fileSizeKey]
        guard let urls = try? FileManager.default.contentsOfDirectory(
            at: dir,
            includingPropertiesForKeys: keys,
            options: [.skipsHiddenFiles]
        ) else { return [] }

        return urls.compactMap { url -> JadeCrashLogEntry? in
            guard url.pathExtension == "log" || url.pathExtension == "txt" else { return nil }
            let values = try? url.resourceValues(forKeys: Set(keys))
            let date = values?.contentModificationDate ?? .distantPast
            let size = values?.fileSize ?? 0
            return JadeCrashLogEntry(url: url, modified: date, size: size)
        }
        .sorted { $0.modified > $1.modified }
    }

    static func reportCount() -> Int {
        loadAll().filter { $0.kind == "Crash" || $0.kind == "Exception" }.count
    }

    /// Paste-ready bundle: header, report body, and the tail of the newest
    /// session log for context.
    static func hermesBundle(reports: [JadeCrashLogEntry], session: JadeCrashLogEntry?, limit: Int = 90_000) -> String {
        var out = ""
        out += "Jade diagnostics bundle\n"
        out += "device: \(UIDevice.current.model) — iOS \(UIDevice.current.systemVersion)\n"
        out += "app: \(Bundle.main.bundleIdentifier ?? "?") \(Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?")\n"
        out += "generated: \(DateFormatter.localizedString(from: Date(), dateStyle: .medium, timeStyle: .medium))\n"

        let interesting = reports.filter { $0.kind == "Crash" || $0.kind == "Exception" }
        let picked = interesting.isEmpty ? Array(reports.prefix(1)) : Array(interesting.prefix(4))

        for entry in picked {
            out += "\n\n===== \(entry.name) (\(entry.sizeText)) =====\n"
            out += entry.text
            if out.count > limit { break }
        }

        if let session, let tail = tailOfFile(session.url, bytes: 24_000) {
            out += "\n\n===== session log tail: \(session.name) =====\n"
            out += tail
        }

        if out.count > limit {
            out = String(out.prefix(limit)) + "\n…(truncated)"
        }
        return out
    }

    private static func tailOfFile(_ url: URL, bytes: UInt64) -> String? {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? handle.close() }
        let size = (try? handle.seekToEnd()) ?? 0
        let start = size > bytes ? size - bytes : 0
        try? handle.seek(toOffset: start)
        guard let data = try? handle.readToEnd(), !data.isEmpty else { return nil }
        return String(decoding: data, as: UTF8.self)
    }
}

// MARK: - List

struct JadeCrashLogsView: View {
    @State private var entries: [JadeCrashLogEntry] = []
    @State private var confirmDeleteAll = false
    @State private var copiedNotice: String?

    private var reports: [JadeCrashLogEntry] {
        entries.filter { $0.kind == "Crash" || $0.kind == "Exception" }
    }

    private var sessions: [JadeCrashLogEntry] {
        entries.filter { $0.kind == "Session" }
    }

    var body: some View {
        List {
            if entries.isEmpty {
                Section {
                    ContentUnavailableView(
                        "No logs yet",
                        systemImage: "checkmark.seal",
                        description: Text("Jade has not crashed, and no session log has been written yet.")
                    )
                }
            } else {
                if !reports.isEmpty {
                    Section("Crash & exception reports") {
                        ForEach(reports) { entry in
                            NavigationLink {
                                JadeCrashReportDetail(entry: entry)
                            } label: {
                                row(for: entry)
                            }
                        }
                    }
                }
                if !sessions.isEmpty {
                    Section("Session breadcrumbs") {
                        ForEach(sessions) { entry in
                            NavigationLink {
                                JadeCrashReportDetail(entry: entry)
                            } label: {
                                row(for: entry)
                            }
                        }
                    }
                }
                Section {
                    Button {
                        copyForHermes()
                    } label: {
                        Label("Copy Latest for Hermes", systemImage: "doc.on.doc")
                    }
                    ShareLink(items: entries.map(\.url)) {
                        Label("Share All Logs", systemImage: "square.and.arrow.up")
                    }
                    Button(role: .destructive) {
                        confirmDeleteAll = true
                    } label: {
                        Label("Delete All Logs", systemImage: "trash")
                    }
                } footer: {
                    Text("Logs are stored in the app's Documents folder (JadeCrashLogs). They survive crashes and reinstalls of nothing — deleting the app deletes them.")
                }
            }
        }
        .navigationTitle("Crash Logs")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    reload()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
            }
        }
        .task { reload() }
        .refreshable { reload() }
        .confirmationDialog("Delete all crash logs?", isPresented: $confirmDeleteAll, titleVisibility: .visible) {
            Button("Delete All", role: .destructive) { deleteAll() }
            Button("Cancel", role: .cancel) {}
        }
        .alert("Copied", isPresented: Binding(
            get: { copiedNotice != nil },
            set: { if !$0 { copiedNotice = nil } }
        )) {
            Button("OK", role: .cancel) { copiedNotice = nil }
        } message: {
            Text(copiedNotice ?? "")
        }
    }

    private func row(for entry: JadeCrashLogEntry) -> some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: entry.symbolName)
                .foregroundStyle(entry.kind == "Session" ? .secondary : .orange)
                .frame(width: 22)
            VStack(alignment: .leading, spacing: 3) {
                Text(entry.name)
                    .font(.callout.monospaced())
                    .lineLimit(1)
                    .truncationMode(.middle)
                Text("\(entry.modified.formatted(date: .abbreviated, time: .standard)) · \(entry.sizeText)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 2)
    }

    private func reload() {
        entries = JadeCrashLogEntry.loadAll()
        JadeCrashBoot.breadcrumb("diagnostics: opened crash log list")
    }

    private func copyForHermes() {
        let bundle = JadeCrashLogEntry.hermesBundle(reports: entries, session: sessions.first)
        UIPasteboard.general.string = bundle
        copiedNotice = "Diagnostics bundle copied (\(bundle.count) characters). Paste it into the chat."
    }

    private func deleteAll() {
        for entry in entries {
            try? FileManager.default.removeItem(at: entry.url)
        }
        reload()
    }
}

// MARK: - Detail

struct JadeCrashReportDetail: View {
    let entry: JadeCrashLogEntry

    var body: some View {
        ScrollView {
            Text(entry.text)
                .font(.system(.caption, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(12)
        }
        .navigationTitle(entry.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                ShareLink(item: entry.url) {
                    Image(systemName: "square.and.arrow.up")
                }
            }
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    UIPasteboard.general.string = entry.text
                } label: {
                    Image(systemName: "doc.on.doc")
                }
            }
        }
    }
}

// MARK: - Settings row

struct JadeCrashLogsSettingsRow: View {
    @State private var reportCount = JadeCrashLogEntry.reportCount()

    var body: some View {
        NavigationLink {
            JadeCrashLogsView()
        } label: {
            HStack {
                Label("Crash Logs", systemImage: "exclamationmark.triangle")
                Spacer()
                if reportCount > 0 {
                    Text("\(reportCount)")
                        .font(.caption.weight(.semibold))
                        .monospacedDigit()
                        .foregroundStyle(.white)
                        .padding(.horizontal, 7)
                        .padding(.vertical, 2)
                        .background(Color.orange, in: Capsule())
                }
            }
        }
    }
}
