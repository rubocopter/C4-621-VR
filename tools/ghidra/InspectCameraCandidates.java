// Bounded, offline inspection of the two AC6 candidate unwind ranges.
// The user-supplied game is imported into an ephemeral project with -noanalysis.
// Output contains private disassembly/pseudocode and belongs OUTSIDE the repo.
//@category AC6Research

import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.SourceType;

public class InspectCameraCandidates extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String expected = "e003771e5548bca91882c1d7dfe2df3de97cee05c2c989d0c0e34b036ea610d6";
        if (!expected.equalsIgnoreCase(currentProgram.getExecutableSHA256())) {
            throw new IllegalStateException("Executable hash differs from inspected AC6 build");
        }
        String[] args = getScriptArgs();
        if (args.length != 1) throw new IllegalArgumentException("Supply an external output directory");
        Path out = Path.of(args[0]).toAbsolutePath().normalize();
        Path project = Path.of("E:/Proyectos/ac6_621").toAbsolutePath().normalize();
        if (out.startsWith(project)) throw new IllegalArgumentException("Keep derived game code outside repo");
        Files.createDirectories(out);
        long[][] ranges = {{0x6bfe50L, 0x6bff21L}, {0x151e890L, 0x151ef2bL}};
        DecompInterface decompiler = new DecompInterface();
        try {
            if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException(decompiler.getLastMessage());
            for (long[] range : ranges) {
                Address begin = currentProgram.getImageBase().add(range[0]);
                Address end = currentProgram.getImageBase().add(range[1] - 1);
                AddressSet body = new AddressSet(begin, end);
                DisassembleCommand command = new DisassembleCommand(begin, body, true);
                command.enableCodeAnalysis(false);
                if (!command.applyTo(currentProgram, monitor)) {
                    throw new IllegalStateException("Disassembly failed: " + command.getStatusMsg());
                }
                Function function = getFunctionAt(begin);
                if (function == null) {
                    function = currentProgram.getFunctionManager().createFunction(
                        "candidate_" + Long.toHexString(range[0]), begin, body, SourceType.USER_DEFINED);
                }
                Path file = out.resolve("candidate_" + Long.toHexString(range[0]) + ".txt");
                try (PrintWriter writer = new PrintWriter(Files.newBufferedWriter(file, StandardCharsets.UTF_8))) {
                    writer.println("SHA256=" + expected);
                    writer.println("Scope=manual PE unwind range; no global auto-analysis or runtime execution");
                    writer.println("Body bounds supplied by analyst from .pdata, not discovered by Ghidra");
                    writer.println("Begin=" + begin + " EndInclusive=" + end);
                    writer.println("ASSEMBLY");
                    InstructionIterator instructions = currentProgram.getListing().getInstructions(body, true);
                    int count = 0;
                    while (instructions.hasNext()) {
                        Instruction instruction = instructions.next();
                        writer.println(instruction.getAddress() + " " + instruction);
                        count++;
                    }
                    writer.println("InstructionCount=" + count);
                    decompiler.flushCache();
                    DecompileResults result = decompiler.decompileFunction(function, 30, monitor);
                    writer.println("DecompileComplete=" + result.decompileCompleted());
                    writer.println("Message=" + result.getErrorMessage());
                    if (result.decompileCompleted()) {
                        writer.println("PSEUDOCODE (inference, not original source)");
                        writer.println(result.getDecompiledFunction().getC());
                    }
                    println("AC6_CANDIDATE " + begin + " instructions=" + count
                        + " decompiled=" + result.decompileCompleted() + " output=" + file);
                }
            }
        } finally {
            decompiler.dispose();
        }
    }
}
