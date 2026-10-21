//
//  bad_queryApp.swift
//  bad_query
//
//  Created by Taj C on 8/10/26.
//

import SwiftUI

@main
struct bad_queryApp: App {
    @StateObject private var state = AppState()
    @Environment(\.scenePhase) private var scenePhase

    init() {
        // Install crash handlers before any UI or sandbox work runs, so a fatal
        // signal anywhere in the app still produces a report on disk.
        JadeCrashBoot.start()
    }

    var body: some Scene {
        WindowGroup {
            BQRootView()
                .environmentObject(state)
                .overlay {
                    if state.show_respring {
                        RespringView()
                            .brightness(-1.0)
                            .ignoresSafeArea()
                    }
                }
        }
        .onChange(of: scenePhase) { _, phase in
            switch phase {
            case .active:
                JadeCrashMarkSessionActive()
                JadeCrashBoot.breadcrumb("scene: active")
            case .background:
                JadeCrashBoot.breadcrumb("scene: background")
                JadeCrashMarkCleanExit()
            case .inactive:
                JadeCrashBoot.breadcrumb("scene: inactive")
            @unknown default:
                break
            }
        }
    }
}
