{
  inputs = {
    hyprland.url = "git+https://github.com/hyprwm/Hyprland?submodules=1&ref=refs/tags/v0.55.4";
    nix-filter.url = "github:numtide/nix-filter";
  };

  outputs = {
    self,
    hyprland,
    nix-filter,
    ...
  }: let
    inherit (hyprland.inputs) nixpkgs;
    forHyprlandSystems = fn:
      nixpkgs.lib.genAttrs (builtins.attrNames hyprland.packages) (
        system: fn system nixpkgs.legacyPackages.${system}
      );
  in {
    packages = forHyprlandSystems (system: pkgs: let
      hyprlandPackage = hyprland.packages.${system}.hyprland;
    in rec {
      hypr-canvas = pkgs.stdenv.mkDerivation {
        pname = "hypr-canvas";
        version = "0.4.7-v055";
        src = nix-filter.lib {
          root = ./.;
          include = [
            "src"
            ./Makefile
          ];
        };

        nativeBuildInputs = with pkgs; [pkg-config];
        buildInputs = [hyprlandPackage.dev] ++ hyprlandPackage.buildInputs;

        installPhase = ''
          mkdir -p $out/lib
          install ./hypr-canvas.so $out/lib/libhypr-canvas.so
        '';

        meta = with pkgs.lib; {
          homepage = "https://github.com/RomeoCavazza/hypr-canvas";
          description = "VXWM-style infinite canvas plugin for Hyprland v0.55.4";
          license = licenses.mit;
          platforms = platforms.linux;
        };
      };

      default = hypr-canvas;
    });

    devShells = forHyprlandSystems (system: pkgs: {
      default = pkgs.mkShell {
        name = "hypr-canvas-dev";
        nativeBuildInputs = with pkgs; [clang-tools_16];
        inputsFrom = [self.packages.${system}.hypr-canvas];
      };
    });
  };
}
