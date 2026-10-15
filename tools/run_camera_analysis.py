"""Run installed Ghidra against two static AC6 camera candidates.

Uses the installed launch.properties VM arguments through argv rather than the
Windows batch wrapper (which fails with the parentheses in Program Files (x86)).
Private derived disassembly goes outside the project in LocalAppData.
"""

from pathlib import Path
import datetime
import os
import subprocess


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    base = Path(r"C:\Users\Oni\Tools\reverse-engineering\ghidra_12.1.4_PUBLIC")
    java = Path(r"C:\Users\Oni\Tools\reverse-engineering\jdk-21.0.12.1+1\bin\java.exe")
    binary = Path(r"C:\Program Files (x86)\Steam\steamapps\common\ARMORED CORE VI FIRES OF RUBICON\Game\armoredcore6.exe")
    vmargs = []
    for line in (base / "support/launch.properties").read_text(encoding="utf-8").splitlines():
        if line.startswith(("VMARGS=", "VMARGS_WINDOWS=")):
            vmargs.append(line.split("=", 1)[1])
    lab = Path(os.environ["LOCALAPPDATA"]) / "ac6-vr-analysis"
    lab.mkdir(parents=True, exist_ok=True)
    run = "camera_" + datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d_%H%M%S_%f")
    output = lab / (run + "-output")
    argv = [str(java), *vmargs, "-Xmx2G", "-Djava.awt.headless=true", "-cp",
            str(base / "Ghidra/Framework/Utility/lib/Utility.jar"), "ghidra.Ghidra",
            "ghidra.app.util.headless.AnalyzeHeadless", str(lab), run,
            "-import", str(binary), "-noanalysis", "-scriptPath", str(root / "tools/ghidra"),
            "-postScript", "InspectCameraCandidates.java", str(output), "-deleteProject"]
    print(f"Private output: {output}", flush=True)
    raise SystemExit(subprocess.call(argv))
