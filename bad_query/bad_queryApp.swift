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

    init() {
        // Install crash handlers before any UI or sandbox work runs, so a fatal
        // signal anywhere in the app still produces a report on disk.
        JadeCrashBoot.start()
    }

    var body: some Scene {
        WindowGroup {
            BQRootView()
                .environmentObject(state)
                .background(JadeScenePhaseObserver())
                .overlay {
                    if state.show_respring {
                        RespringView()
                            .brightness(-1.0)
                            .ignoresSafeArea()
                    }
                }
        }
    }
}
