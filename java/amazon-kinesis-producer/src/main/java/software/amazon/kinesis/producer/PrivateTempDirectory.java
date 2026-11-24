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

import java.io.IOException;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.attribute.PosixFileAttributes;
import java.nio.file.attribute.PosixFilePermission;
import java.nio.file.attribute.PosixFilePermissions;
import java.util.Set;

/**
 * Creates the directory KPL extracts its native binary, certificates and pipes into.
 * On POSIX systems the directory is per-uid, mode 0700, and must be owned by the current user,
 * so other local users cannot replace its contents.
 */
final class PrivateTempDirectory {
    private static final Set<PosixFilePermission> OWNER_ONLY = PosixFilePermissions.fromString("rwx------");

    private PrivateTempDirectory() {
    }

    static Path prepare(Path parent, String name) throws IOException {
        Files.createDirectories(parent);
        if (!parent.getFileSystem().supportedFileAttributeViews().contains("unix")) {
            return Files.createDirectories(parent.resolve(name));
        }

        int uid = currentUid(parent);
        Path dir = parent.resolve(name + "-" + uid);
        try {
            Files.createDirectory(dir, PosixFilePermissions.asFileAttribute(OWNER_ONLY));
        } catch (FileAlreadyExistsException e) {
            // Reuse it only if it passes the checks below.
        }

        PosixFileAttributes attrs = Files.readAttributes(dir, PosixFileAttributes.class, LinkOption.NOFOLLOW_LINKS);
        if (!attrs.isDirectory()) {
            throw new IOException(dir + " is not a directory. Remove it or set TempDirectory to a private directory.");
        }
        int owner = (Integer) Files.getAttribute(dir, "unix:uid", LinkOption.NOFOLLOW_LINKS);
        if (owner != uid) {
            throw new IOException(dir + " is owned by uid " + owner + ", not " + uid
                    + ". Remove it or set TempDirectory to a private directory.");
        }
        if (!attrs.permissions().equals(OWNER_ONLY)) {
            Files.setPosixFilePermissions(dir, OWNER_ONLY);
        }
        return dir;
    }

    private static int currentUid(Path parent) throws IOException {
        Path probe = Files.createTempFile(parent, "kpl", ".uid");
        try {
            return (Integer) Files.getAttribute(probe, "unix:uid");
        } finally {
            Files.deleteIfExists(probe);
        }
    }
}
