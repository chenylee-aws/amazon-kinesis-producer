/*
 * Copyright 2026 Amazon.com, Inc. or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package software.amazon.kinesis.producer;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assume.assumeTrue;

import java.io.IOException;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFilePermissions;

import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class PrivateTempDirectoryTest {
    private static final String NAME = "amazon-kinesis-producer-native-binaries";

    @Rule
    public TemporaryFolder temp = new TemporaryFolder();

    private Path parent;
    private Path expected;

    @Before
    public void setup() throws IOException {
        assumeTrue(FileSystems.getDefault().supportedFileAttributeViews().contains("unix"));
        parent = temp.getRoot().toPath();
        int uid = (Integer) Files.getAttribute(parent, "unix:uid");
        expected = parent.resolve(NAME + "-" + uid);
    }

    @Test
    public void createsOwnerOnlyDirectory() throws Exception {
        Path dir = PrivateTempDirectory.prepare(parent, NAME);

        assertEquals(expected, dir);
        assertEquals("rwx------", PosixFilePermissions.toString(Files.getPosixFilePermissions(dir)));
    }

    @Test
    public void createsMissingParent() throws Exception {
        Path nested = parent.resolve("a/b");

        Path dir = PrivateTempDirectory.prepare(nested, NAME);

        assertTrue(Files.isDirectory(dir));
        assertEquals(nested, dir.getParent());
    }

    @Test
    public void tightensOwnExistingDirectory() throws Exception {
        Files.createDirectory(expected);
        Files.setPosixFilePermissions(expected, PosixFilePermissions.fromString("rwxrwxrwx"));
        Path kept = Files.createFile(expected.resolve("kinesis_producer_abc"));

        Path dir = PrivateTempDirectory.prepare(parent, NAME);

        assertEquals(expected, dir);
        assertEquals("rwx------", PosixFilePermissions.toString(Files.getPosixFilePermissions(dir)));
        assertTrue(Files.exists(kept));
    }

    @Test(expected = IOException.class)
    public void rejectsSymlink() throws Exception {
        Path target = Files.createDirectory(parent.resolve("elsewhere"));
        Files.createSymbolicLink(expected, target);

        PrivateTempDirectory.prepare(parent, NAME);
    }

    @Test(expected = IOException.class)
    public void rejectsRegularFile() throws Exception {
        Files.createFile(expected);

        PrivateTempDirectory.prepare(parent, NAME);
    }
}
