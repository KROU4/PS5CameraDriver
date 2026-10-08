# Release signing through SignPath Foundation

**English** · [Русский](README.ru.md)

The build is ready for signing: on a `v*` tag the [build.yml](../.github/workflows/build.yml)
workflow sends the Windows package to SignPath and publishes the signed archive in the release.
Until SignPath is set up, releases are published unsigned.

## One-time setup by the repository owner

1. Enable two-factor authentication on GitHub (a SignPath Foundation condition for the whole team).
2. Apply for free code signing: <https://signpath.org/apply> (repository, GPL-3.0 license, an
   existing release, this page and the "Code signing policy" section of the README).
3. Once approved, in SignPath.io:
   - a project with the slug `PS5CameraDriver`;
   - the **GitHub.com** trusted build system linked to the project; install the
     [SignPath GitHub App](https://github.com/apps/signpath) for this repository;
   - the artifact configuration from [artifact-configuration.xml](artifact-configuration.xml);
   - a `release-signing` signing policy with manual approval;
   - an API token of a CI user allowed to submit signing requests.
4. In the GitHub repository settings (Settings → Secrets and variables → Actions):
   - secret `SIGNPATH_API_TOKEN` — the token from step 3;
   - variable `SIGNPATH_ORGANIZATION_ID` — the SignPath organization ID;
   - with other slugs, variables `SIGNPATH_PROJECT_SLUG` and `SIGNPATH_POLICY_SLUG`.

After that every `v*` tag creates a signing request; approve it in SignPath and the build continues
on its own (it waits up to a day).
