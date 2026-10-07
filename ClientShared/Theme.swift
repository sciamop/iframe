import SwiftUI

extension Color {
    static let iframeTeal = Color(red: 0x2E / 255, green: 0xE6 / 255, blue: 0xD6 / 255)  // #2EE6D6
    /// Text on teal: the accent is light, so dark text reads better than white.
    static let iframeInk = Color(red: 0x04 / 255, green: 0x16 / 255, blue: 0x1A / 255)
}

/// The cursor mark streaking up and to the right, with a short motion trail
/// (iFrame's take on Pinry's falling-pin loader).
struct IFrameLoadingView: View {
    let size: CGFloat
    @State private var progress: CGFloat = -1

    var body: some View {
        ZStack {
            ForEach(0..<3, id: \.self) { index in
                Image("IFrameMark")
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(width: size * 0.7, height: size * 0.7)
                    .opacity(1.0 - CGFloat(index) * 0.3)
                    .offset(x: (progress - CGFloat(index) * 0.18) * size,
                            y: -(progress - CGFloat(index) * 0.18) * size)
            }
        }
        .frame(width: size, height: size)
        .clipped()
        .drawingGroup()
        .onAppear {
            progress = -1
            withAnimation(.easeInOut(duration: 0.6).repeatForever(autoreverses: false)) {
                progress = 1
            }
        }
    }
}
