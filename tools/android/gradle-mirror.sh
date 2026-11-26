#!/usr/bin/env bash

# Gradle builds both Qt Java libraries and application APKs.
configure_android_gradle_mirror() {
  local gradle_home="${GRADLE_USER_HOME:-$HOME/.gradle}"
  mkdir -p "$gradle_home/init.d"
  cat > "$gradle_home/init.d/spool-maven-mirror.gradle" <<'GRADLE'
import org.gradle.api.initialization.resolve.RepositoriesMode

def mirror = "https://maven-central.storage-download.googleapis.com/maven2/"

beforeSettings { settings ->
    settings.dependencyResolutionManagement.repositories {
        maven { url = uri(mirror) }
    }
}
settingsEvaluated { settings ->
    def projectRepositoriesAllowed = settings.dependencyResolutionManagement.repositoriesMode.get() == RepositoriesMode.PREFER_PROJECT
    gradle.allprojects {
        buildscript.repositories {
            google()
            maven { url = uri(mirror) }
        }
        if (projectRepositoriesAllowed) {
            repositories {
                google()
                maven { url = uri(mirror) }
            }
        }
    }
}
GRADLE
}
