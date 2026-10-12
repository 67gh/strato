package org.stratoemu.strato.utils;

import java.io.File;
import java.io.IOException;
import java.nio.file.FileVisitResult;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.SimpleFileVisitor;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.Locale;

/** Removes only the two generated pipeline caches for one NACP save-data owner. */
public final class ShaderCacheFiles {
    private ShaderCacheFiles() {}

    public static boolean delete(File publicFiles, String titleId) throws IOException {
        if (titleId == null || !titleId.matches("[0-9a-fA-F]{16}"))
            throw new IllegalArgumentException("Missing or invalid cache owner ID");

        String owner = titleId.toUpperCase(Locale.ROOT);
        boolean found = false;
        for (String cache : new String[]{"graphics_pipeline_cache/" + owner,
                "graphics_pipeline_cache/" + owner + ".staging", "vk_graphics_pipeline_cache/" + owner}) {
            Path target = new File(publicFiles, cache).toPath();
            Path parent = target.getParent();
            if (Files.isSymbolicLink(parent))
                throw new IOException("Cache directory is a symbolic link");
            if (!Files.exists(target, LinkOption.NOFOLLOW_LINKS))
                continue;
            found = true;
            // walkFileTree does not follow links; a link is deleted, never its destination.
            Files.walkFileTree(target, new SimpleFileVisitor<Path>() {
                @Override
                public FileVisitResult visitFile(Path file, BasicFileAttributes attributes) throws IOException {
                    Files.delete(file);
                    return FileVisitResult.CONTINUE;
                }

                @Override
                public FileVisitResult postVisitDirectory(Path directory, IOException failure) throws IOException {
                    if (failure != null) throw failure;
                    Files.delete(directory);
                    return FileVisitResult.CONTINUE;
                }
            });
        }
        return found;
    }
}
