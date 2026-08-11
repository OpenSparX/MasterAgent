# Homebrew formula for sparx.
#
# Lives in a tap (homebrew-masteragent repo) as Formula/sparx.rb:
#   brew install OpenSparX/masteragent/sparx
#
# Binary-only formula rather than build-from-source. Rationale: a source build
# needs CMake plus the full kernel tree, which turns a 44 KB download into a
# multi-minute compile for no user-visible benefit. The artifacts are already
# built and checksummed by the release workflow.
#
# The sha256 values and version below are rewritten by
# scripts/update_packaging.sh on each release — do not hand-edit them.
class Sparx < Formula
  desc "On-device Agent framework for Qualcomm and ARM platforms"
  homepage "https://github.com/OpenSparX/MasterAgent"
  version "2.1.0"
  license "Apache-2.0"

  on_macos do
    on_arm do
      url "https://github.com/OpenSparX/MasterAgent/releases/download/v2.1.0/sparx-2.1.0-darwin-arm64.tar.gz"
      sha256 "df9ea8be2d9b4fb0b2e4c80f02769a3307601849c9305d74679cbcac329adb23"
    end
    on_intel do
      url "https://github.com/OpenSparX/MasterAgent/releases/download/v2.1.0/sparx-2.1.0-darwin-x64.tar.gz"
      sha256 "REPLACE_DARWIN_X64_SHA256"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/OpenSparX/MasterAgent/releases/download/v2.1.0/sparx-2.1.0-linux-arm64.tar.gz"
      sha256 "REPLACE_LINUX_ARM64_SHA256"
    end
    on_intel do
      url "https://github.com/OpenSparX/MasterAgent/releases/download/v2.1.0/sparx-2.1.0-linux-x64.tar.gz"
      sha256 "REPLACE_LINUX_X64_SHA256"
    end
  end

  def install
    bin.install "bin/sparx"
    # Not installed: libGenie.so and the QNN runtime. Those are Qualcomm-
    # licensed and must be obtained from the Qualcomm SDK by the developer;
    # sparx resolves them with dlopen at runtime. `sparx doctor` reports
    # whether they are present and correctly configured.
  end

  def caveats
    <<~EOS
      NPU acceleration needs the Qualcomm AI Engine Direct runtime, which is
      not redistributable and is therefore not bundled. Without it sparx runs
      on CPU via llama.cpp.

      Check your setup with:
        sparx doctor
    EOS
  end

  test do
    # Homebrew runs this after install; it must not need a network or a device.
    assert_match "sparx #{version}", shell_output("#{bin}/sparx version")
    assert_match "Usage:", shell_output("#{bin}/sparx --help")
    system "#{bin}/sparx", "init", "brew-test-agent"
    assert_predicate testpath/"brew-test-agent/agent.yaml", :exist?
  end
end
