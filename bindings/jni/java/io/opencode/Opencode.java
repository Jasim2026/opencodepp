package io.opencode;

import com.sun.net.httpserver.HttpServer;

import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Phase 12 task 6: JNI binding harness. Drives one engine session through
 * {@code io.opencode.Opencode} native methods (JNI glue over the frozen C
 * ABI) against an in-JVM mock SSE server (JDK {@code com.sun.net.httpserver}).
 *
 * <p>Run:
 * <pre>
 *   javac io/opencode/Opencode.java
 *   java -Djava.library.path=&lt;dir&gt; io.opencode.Opencode
 * </pre>
 */
public final class Opencode {
    static {
        System.loadLibrary("opencodepp_jni");
    }

    /* Native surface (mirrors include/opencode/opencode.h). */
    public static native int abiVersion();

    public static native long engineCreate(String workspace, String baseUrl,
                                           int toolPolicy);

    public static native int engineRun(long handle, String prompt);

    public static native int engineCancel(long handle);

    public static native int engineDestroy(long handle);

    public static native int metricsSnapshot(long handle);

    public static native String memoryWrite(long handle, int kind, String key,
                                            String value);

    /* Status / policy / memory kind constants (see opencode.h). */
    public static final int OK = 0;
    public static final int POLICY_ALLOW = 2;
    public static final int MEMORY_FACT = 1;

    private static final String[][] FRAMES = {
        {
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c\","
                + "\"function\":{\"name\":\"file.write\",\"arguments\":"
                + "\"{\\\"path\\\":\\\"jni.txt\\\",\\\"content\\\":"
                + "\\\"hello jni\\\\n\\\",\\\"create\\\":true}\"}}]},"
                + "\"finish_reason\":\"tool_calls\"}]}",
            "data: {\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8}}"
        },
        {
            "data: {\"choices\":[{\"delta\":{\"content\":\"done\"},"
                + "\"finish_reason\":null}]}",
            "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}",
            "data: {\"usage\":{\"prompt_tokens\":40,\"completion_tokens\":2}}"
        }
    };

    private Opencode() {}

    private static HttpServer startMock(AtomicInteger rounds) throws IOException {
        HttpServer server = HttpServer.create(new InetSocketAddress(0), 0);
        server.createContext("/chat/completions", exchange -> {
            try {
                exchange.getRequestBody().readAllBytes();
                String[] frames = FRAMES[rounds.getAndIncrement() == 0 ? 0 : 1];
                byte[] bytes = (String.join("\n\n", frames) + "\n\n")
                        .getBytes(StandardCharsets.UTF_8);
                exchange.getResponseHeaders().set("Content-Type",
                        "text/event-stream");
                exchange.sendResponseHeaders(200, bytes.length);
                try (OutputStream os = exchange.getResponseBody()) {
                    os.write(bytes);
                }
            } catch (IOException e) {
                exchange.close();
            }
        });
        server.start();
        return server;
    }

    private static int failures = 0;

    private static void check(boolean cond, String label) {
        System.out.println("  " + label + ": " + (cond ? "OK" : "FAIL"));
        if (!cond) {
            failures++;
        }
    }

    public static void main(String[] args) throws Exception {
        AtomicInteger rounds = new AtomicInteger();
        HttpServer mock = startMock(rounds);
        Path ws = Files.createTempDirectory("opencode_jni_ws_");
        try {
            check(abiVersion() == 1, "abiVersion");

            long h = engineCreate(ws.toString(),
                    "http://127.0.0.1:" + mock.getAddress().getPort(),
                    POLICY_ALLOW);
            check(h != 0, "engineCreate");

            if (h != 0) {
                int st = engineRun(h, "write jni.txt");
                check(st == OK, "engineRun status ok (got " + st + ")");

                Path out = ws.resolve("jni.txt");
                check(Files.isRegularFile(out), "file.write applied");
                if (Files.isRegularFile(out)) {
                    String content = Files.readString(out, StandardCharsets.UTF_8);
                    check("hello jni\n".equals(content), "file content");
                }

                check(metricsSnapshot(h) >= 1, "metricsSnapshot");
                String id = memoryWrite(h, MEMORY_FACT, "j_key", "j_value");
                check(id != null && !id.isEmpty(), "memoryWrite id");
                check(engineCancel(h) == OK, "engineCancel idle -> OK");
                engineDestroy(h);
            }
        } finally {
            mock.stop(0);
        }

        System.out.println("jni_binding_test: " + (failures == 0
                ? "all sections OK" : failures + " failure(s)"));
        if (failures != 0) {
            System.exit(1);
        }
    }
}
