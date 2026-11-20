# Third-party notices

Corner Walk's own code is under the MIT license in [`LICENSE`](LICENSE). The
parts below come from other projects and keep their own notices.

## Starter game artwork

The game template in `game/` began as the starter example of the
[ModRetro Chromatic plugin for Codex](https://github.com/OpenAI-Partners/ModRetro-Chromatic-Plugin-for-Codex)
(version 1.0.34). These files are unchanged from that example, and copies of
them appear in the example projects under `game/artifacts/`:

- `game/assets/backgrounds/workshop.png`
- `game/assets/sprites/workshop_player.png`
- `game/assets/fonts/bench-mono.png` and `bench-mono.json`
- `game/assets/ui/frame.png` and `cursor.png`

They are used under that project's MIT license:

```
MIT License

Copyright (c) 2026 Eric Provencher

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## GB Studio project format

The GB Studio project files (`*.gbsproj`, `*.gbsres`) follow GB Studio's native
project format. Its MIT notice, copyright Chris Maltby, is kept in
[`game/LICENSE`](game/LICENSE).

## Compiled cartridges

The `.gbc` files under `game/artifacts/` were compiled with GB Studio and
contain two runtimes:

- GBVM, under the MIT license in [`licenses/GBVM-MIT.txt`](licenses/GBVM-MIT.txt).
- GBDK, under GPL version 2 with its linking exception, in
  [`licenses/GBDK-GPL-2.0-linking-exception.txt`](licenses/GBDK-GPL-2.0-linking-exception.txt).
