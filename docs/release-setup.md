# Release setup

What the user does once, so releases are signed and reach every package manager: the signing key
for the Linux repositories, the Homebrew tap, Azure's code signing, the winget fork, the VS Code
Marketplace and Open VSX, and the R2 bucket. docs/tooling.md's Releases says what each is for;
release.yml makes the release, and publish.yml sends a published one on.

Every secret and variable goes in hlpdev/FutureBlitz, set with `gh` from a checkout (or in
Settings > Secrets and variables > Actions). A secret is read from stdin, so it never lands in the
shell's history:

```sh
gh secret set NAME -R hlpdev/FutureBlitz < file        # or type it, then Ctrl+D
gh variable set NAME -R hlpdev/FutureBlitz --body value
```

| Name | Kind | Used by | For |
|---|---|---|---|
| `REPO_SIGNING_KEY` | secret | release.yml, publish.yml | signing the .rpms, and the apt and dnf repositories |
| `HOMEBREW_TAP_TOKEN` | secret | publish.yml | pushing futureblitz.rb to the tap |
| `HOMEBREW_TAP` | variable, optional | publish.yml | the tap, `hlpdev/homebrew-tap` by default |
| `AZURE_TENANT_ID` | secret | release.yml | Windows signing |
| `AZURE_CLIENT_ID` | secret | release.yml | Windows signing |
| `AZURE_CLIENT_SECRET` | secret | release.yml | Windows signing |
| `AZURE_SIGNING_ENDPOINT` | variable | release.yml | Windows signing |
| `AZURE_SIGNING_ACCOUNT` | variable | release.yml | Windows signing |
| `AZURE_SIGNING_PROFILE` | variable | release.yml | Windows signing |
| `WINGET_TOKEN` | secret | publish.yml | the pull request to microsoft/winget-pkgs |
| `WINGET_FORK` | variable, optional | publish.yml | the fork, `hlpdev/winget-pkgs` by default |
| `VSCE_PAT` | secret | publish.yml | the VS Code Marketplace |
| `OVSX_PAT` | secret | publish.yml | Open VSX |
| `R2_ACCOUNT_ID` | secret | publish.yml | R2's endpoint |
| `R2_ACCESS_KEY_ID` | secret | publish.yml | R2 |
| `R2_SECRET_ACCESS_KEY` | secret | publish.yml | R2 |
| `R2_BUCKET` | variable | publish.yml | the bucket dl.futureblitz.dev serves |

Without the Azure ones, Windows' files are unsigned, with a warning; without REPO_SIGNING_KEY,
the .rpms are, and publish.yml's linux job refuses them. Each publish.yml job says which secret it
lacks.

## The repositories' signing key

One RSA 4096 key that only signs, with no passphrase, as GitHub's secrets keep it: nfpm signs the
.rpms with it, and repo.py the apt repository's Release and the dnf repository's repomd.xml. It
doesn't expire, so the sources people add keep working; its revocation certificate is how it's
retired. In Git Bash on Windows, or any shell with gpg:

```sh
export GNUPGHOME="$(mktemp -d)"
gpg --batch --passphrase '' --quick-gen-key "FutureBlitz packages" rsa4096 sign never
fpr="$(gpg --list-keys --with-colons | awk -F: '/^fpr:/ { print $10; exit }')"
echo "$fpr"
gpg --armor --export-secret-keys "$fpr" > futureblitz-signing.asc
gpg --armor --export "$fpr" > futureblitz-public.asc
cp "$GNUPGHOME/openpgp-revocs.d/$fpr.rev" futureblitz-revoke.rev
gh secret set REPO_SIGNING_KEY -R hlpdev/FutureBlitz < futureblitz-signing.asc
```

Keep futureblitz-signing.asc and futureblitz-revoke.rev somewhere safe and offline, like a
password manager, then delete them here with the throwaway home: `rm -rf "$GNUPGHOME"
futureblitz-signing.asc futureblitz-revoke.rev`. futureblitz-public.asc is public; the
repositories serve it as futureblitz.asc (and futureblitz.gpg, binary), and the fingerprint can
go on the site's install page.

## The Homebrew tap

A public repo named `homebrew-tap`, which `brew install hlpdev/tap/futureblitz` finds:

```sh
gh repo create hlpdev/homebrew-tap --public --add-readme \
  --description "Homebrew formulae for FutureBlitz"
```

Then a fine-grained token for it: GitHub > Settings > Developer settings > Personal access tokens
> Fine-grained tokens > Generate new token, with Repository access "Only select repositories",
hlpdev/homebrew-tap, and Permissions > Contents "Read and write". Give it an expiry you'll
remember, and set it:

```sh
gh secret set HOMEBREW_TAP_TOKEN -R hlpdev/FutureBlitz
```

## Windows signing (Azure Artifact Signing)

Artifact Signing was called Trusted Signing; the portal and the role may show either name.

1. In the Azure portal, make a resource group, and in it an **Artifact Signing Account**
   (Basic is enough), in a region near you. Its Overview shows the **Account URI**, like
   `https://eus.codesigning.azure.net/`: that's the endpoint.
2. In the account, **Identity validation** > New > Public, as an individual or an organization,
   and wait for it to be approved (a few days at most). The publisher name it validates is what
   Windows shows.
3. **Certificate profiles** > Create > Public Trust, with that identity. Its name is the profile.
4. An app for GitHub to sign as, with `az` (`az login` first):

   ```sh
   app="$(az ad app create --display-name futureblitz-signing --query appId -o tsv)"
   az ad sp create --id "$app"
   az ad app credential reset --id "$app" --display-name github --years 2 \
     --query "{tenant: tenant, client: appId, secret: password}"
   ```

   It prints the tenant, the client, and the secret; the secret isn't shown again.
5. Let it sign with the profile:

   ```sh
   az role assignment create --assignee "$app" \
     --role "Artifact Signing Certificate Profile Signer" \
     --scope "/subscriptions/<subscription>/resourceGroups/<group>/providers/Microsoft.CodeSigning/codeSigningAccounts/<account>/certificateProfiles/<profile>"
   ```

   (`Trusted Signing Certificate Profile Signer` where the old name's still used.)
6. Set them:

   ```sh
   gh secret set AZURE_TENANT_ID -R hlpdev/FutureBlitz
   gh secret set AZURE_CLIENT_ID -R hlpdev/FutureBlitz
   gh secret set AZURE_CLIENT_SECRET -R hlpdev/FutureBlitz
   gh variable set AZURE_SIGNING_ENDPOINT -R hlpdev/FutureBlitz --body https://eus.codesigning.azure.net/
   gh variable set AZURE_SIGNING_ACCOUNT -R hlpdev/FutureBlitz --body <account>
   gh variable set AZURE_SIGNING_PROFILE -R hlpdev/FutureBlitz --body <profile>
   ```

The client secret expires (2 years above); a new one is `az ad app credential reset` again and
`gh secret set AZURE_CLIENT_SECRET`.

## The winget fork

publish.yml opens the pull request to microsoft/winget-pkgs from a fork of it:

```sh
gh repo fork microsoft/winget-pkgs --clone=false
```

Then a **classic** token, as a fine-grained one can't open a pull request on a repo you don't
own: GitHub > Settings > Developer settings > Personal access tokens > Tokens (classic) >
Generate new token, with the `public_repo` scope.

```sh
gh secret set WINGET_TOKEN -R hlpdev/FutureBlitz
```

The first pull request has Microsoft's CLA bot ask you to agree to its CLA, in a comment, once;
winget-pkgs' moderators review a new package before it merges.

## The VS Code Marketplace and Open VSX

The extension's publisher, in editors/vscode/package.json, is `futureblitz`; the Marketplace's
publisher and Open VSX's namespace have to be that.

1. **The Marketplace**: at https://marketplace.visualstudio.com/manage, signed in with a
   Microsoft account, Create publisher, with the ID `futureblitz` and the name FutureBlitz.
2. A token for it, from Azure DevOps: at https://dev.azure.com (make an organization if there's
   none), User settings > Personal access tokens > New Token, with Organization "All accessible
   organizations", and Scopes > Custom defined > Marketplace > Manage. Check it, then set it:

   ```sh
   cd editors/vscode && npm ci
   VSCE_PAT=<token> npx vsce verify-pat futureblitz
   gh secret set VSCE_PAT -R hlpdev/FutureBlitz
   ```

3. **Open VSX**: at https://open-vsx.org, log in with GitHub, link an Eclipse account in your
   profile, and sign the Publisher Agreement there. Then Settings > Access Tokens > Generate New
   Token, and make the namespace with it:

   ```sh
   npx --yes ovsx@1.3.0 create-namespace futureblitz -p <token>
   gh secret set OVSX_PAT -R hlpdev/FutureBlitz
   ```

   An issue at https://github.com/EclipseFdn/open-vsx.org asking to verify the namespace's owner
   marks it as yours.

## The R2 bucket

The bucket that dl.futureblitz.dev serves (docs/website.md); if futureblitz-web made it already,
use that one. The repositories go under its `apt/` and `rpm/`. With wrangler, logged in to the
Cloudflare account that has futureblitz.dev:

```sh
npx wrangler r2 bucket create futureblitz-dl
npx wrangler r2 bucket domain add futureblitz-dl --domain dl.futureblitz.dev --zone-id <futureblitz.dev's zone id>
```

Then its keys: the dashboard's R2 > Manage API tokens > Create API token, with Permissions "Object
Read & Write", and Specify bucket(s) > futureblitz-dl. It shows an Access Key ID and a Secret
Access Key once; the account ID is on R2's overview.

```sh
gh secret set R2_ACCOUNT_ID -R hlpdev/FutureBlitz
gh secret set R2_ACCESS_KEY_ID -R hlpdev/FutureBlitz
gh secret set R2_SECRET_ACCESS_KEY -R hlpdev/FutureBlitz
gh variable set R2_BUCKET -R hlpdev/FutureBlitz --body futureblitz-dl
```

The site serves install.ps1 as https://futureblitz.dev/install.ps1 (M9), with `Content-Type:
text/plain; charset=utf-8`.

## A release, then

1. Set CMakeLists.txt's version to the release's, push the tag (`git tag v1.0.0 && git push
   origin v1.0.0`), and wait for release.yml's draft.
2. Check the draft, then publish it.
3. `gh workflow run publish.yml -f version=1.0.0 -f winget=true -f homebrew=true -f linux=true -f
   vscode=true`, or only some of them.
