pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    // The app declares no repositories of its own, so everything resolves from
    // here and a stray repository in a module is an error rather than a
    // surprise source of a dependency.
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "bodyclock"
include(":app")
