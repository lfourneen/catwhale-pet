{
  description = "Catwhale Pet, a native Wayland desktop pet";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      eachSystem = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      version = nixpkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION);
    in
    {
      packages = eachSystem (
        pkgs:
        let
          catwhale-pet = pkgs.stdenv.mkDerivation {
            pname = "catwhale-pet";
            inherit version;
            src = pkgs.lib.fileset.toSource {
              root = ./.;
              fileset = pkgs.lib.fileset.unions [
                ./CMakeLists.txt
                ./VERSION
                ./LICENSE
                ./assets
                ./src
                ./tests
              ];
            };

            nativeBuildInputs = with pkgs; [
              cmake
              ninja
              pkg-config
              qt6.wrapQtAppsHook
            ];
            buildInputs = with pkgs; [
              ffmpeg-headless
              qt6.qtbase
              qt6.qtdeclarative
              qt6.qtwayland
              vulkan-headers
            ];

            preFixup = ''
              qtWrapperArgs+=(
                --suffix PATH : ${
                  pkgs.lib.makeBinPath [
                    pkgs.playerctl
                    pkgs.xdg-utils
                  ]
                }
                --suffix LD_LIBRARY_PATH : ${
                  pkgs.lib.makeLibraryPath [ pkgs.vulkan-loader ]
                }:/run/opengl-driver/lib
                --set-default CATWHALE_LAUNCHER "$out/bin/catwhale-pet"
              )
            '';

            doCheck = true;
            checkPhase = ''
              runHook preCheck
              ctest --output-on-failure
              runHook postCheck
            '';

            meta = with pkgs.lib; {
              description = "Transparent animated desktop pet for Linux and Wayland";
              homepage = "https://github.com/timevisitor/catwhale-pet";
              license = licenses.mit;
              platforms = platforms.linux;
              mainProgram = "catwhale-pet";
            };
          };
        in
        {
          default = catwhale-pet;
          inherit catwhale-pet;
        }
      );

      apps = eachSystem (
        pkgs:
        let
          catwhale-pet = {
            type = "app";
            program = pkgs.lib.getExe self.packages.${pkgs.system}.catwhale-pet;
          };
        in
        {
          default = catwhale-pet;
          inherit catwhale-pet;
        }
      );

      devShells = eachSystem (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.system}.default ];
          packages = [ pkgs.vulkan-loader ];
          shellHook = ''
            export QT_PLUGIN_PATH="${pkgs.qt6.qtbase}/lib/qt-6/plugins:${pkgs.qt6.qtwayland}/lib/qt-6/plugins${"$"}{QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
            export LD_LIBRARY_PATH="${
              pkgs.lib.makeLibraryPath [ pkgs.vulkan-loader ]
            }:/run/opengl-driver/lib${"$"}{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
          '';
        };
      });

      checks = eachSystem (pkgs: {
        inherit (self.packages.${pkgs.system}) catwhale-pet;
      });

      formatter = eachSystem (pkgs: pkgs.nixfmt-rfc-style);
    };
}
