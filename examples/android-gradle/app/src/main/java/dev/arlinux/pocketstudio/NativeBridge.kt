package dev.arlinux.pocketstudio

object NativeBridge {
    init { System.loadLibrary("pocketnative") }
    external fun summary(): String
    external fun render(pixels: IntArray, width: Int, height: Int)
}
