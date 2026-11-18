# Dependências e distribuição

Código do launcher e bridge deste projeto: GPL-3.0-or-later.

- FreeJ2ME-Plus: GPL-3.0-or-later. Preservar licença e avisos, identificar a revisão e fornecer o código correspondente quando distribuir seu binário. A cópia da base é fixada por ID do artefato de release e hash no lock; o runtime do Meteos estável não foi alterado. Não alegar build reproduzível de uma nightly sem fixar a revisão correspondente.
- SDL3: licença zlib, incluída na pasta local de dependências.
- SDL_GameControllerDB: licença zlib, incluída junto ao banco de mapeamentos.
- SDL2: licença zlib, empacotada no runtime N-Gage/EKA2L1 quando utilizado.
- EKA2L1 (emulador Symbian / N-Gage): GPL-2.0-or-later (`https://github.com/EKA2L1/EKA2L1`); empacotado sem ROMs, firmware, jogos ou saves comerciais, acompanhado de Qt 6 (LGPLv3/GPLv3), FFmpeg (LGPLv2.1+/GPL) e OpenSSL (Apache-2.0).
- Java incluído em releases: preservar avisos e licenças do runtime usado pelo jlink.
- ReShade: obter do site oficial. DLL/setup/shaders baixados ficam somente nos experimentos privados. Distribuir apenas o código próprio do preset e instruções para o usuário obter o ReShade, respeitando as instruções do projeto.

Jogos, sistemas Symbian, firmware, sprites e artes comerciais não são fornecidos por este repositório.
