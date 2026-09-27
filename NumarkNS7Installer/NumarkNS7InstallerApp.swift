// NumarkNS7InstallerApp.swift
// Host app that activates / deactivates the NumarkNS7Driver system extension.
// macOS only loads a dext that was activated through OSSystemExtensionManager
// from an app in /Applications, so this app must be copied there first.

import SwiftUI
import SystemExtensions

private let driverIdentifier = "com.andrewabner.ns7.driverkit"

@main
struct NumarkNS7InstallerApp: App {
    var body: some Scene {
        WindowGroup("Numark NS7 Driver") {
            ContentView()
                .frame(minWidth: 420, minHeight: 220)
        }
        .windowResizability(.contentSize)
    }
}

struct ContentView: View {
    @StateObject private var manager = ExtensionManager()

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Numark NS7 Driver").font(.title2).bold()
            Text(manager.status)
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Button("Install Driver") { manager.submit(.activate) }
                    .keyboardShortcut(.defaultAction)
                Button("Uninstall Driver") { manager.submit(.deactivate) }
            }
            .disabled(manager.busy)
        }
        .padding(24)
    }
}

@MainActor
final class ExtensionManager: NSObject, ObservableObject {
    enum Action { case activate, deactivate }

    @Published var status = "Install the driver, then connect the NS7."
    @Published var busy = false

    func submit(_ action: Action) {
        let request: OSSystemExtensionRequest
        switch action {
        case .activate:
            request = .activationRequest(forExtensionWithIdentifier: driverIdentifier, queue: .main)
            status = "Requesting activation…"
        case .deactivate:
            request = .deactivationRequest(forExtensionWithIdentifier: driverIdentifier, queue: .main)
            status = "Requesting removal…"
        }
        request.delegate = self
        busy = true
        OSSystemExtensionManager.shared.submitRequest(request)
    }
}

extension ExtensionManager: OSSystemExtensionRequestDelegate {
    nonisolated func request(_ request: OSSystemExtensionRequest,
                             actionForReplacingExtension existing: OSSystemExtensionProperties,
                             withExtension ext: OSSystemExtensionProperties)
        -> OSSystemExtensionRequest.ReplacementAction {
        .replace
    }

    nonisolated func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        Task { @MainActor in
            self.status = "Approve the driver in System Settings → General → Login Items & Extensions → Driver Extensions."
        }
    }

    nonisolated func request(_ request: OSSystemExtensionRequest,
                             didFinishWithResult result: OSSystemExtensionRequest.Result) {
        Task { @MainActor in
            self.busy = false
            self.status = result == .willCompleteAfterReboot
                ? "Done. Restart your Mac to finish."
                : "Done."
        }
    }

    nonisolated func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        Task { @MainActor in
            self.busy = false
            self.status = "Failed: \(error.localizedDescription)"
        }
    }
}
