{
  description = "Waguri MPV for NixOS";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/f1e5c356f35d07e7b71a6b5e6c9338e0b029e0c0";
    mpv-source = {
      url = "github:mpv-player/mpv/bb4823e5f0eb92a830242e60dfdcc50d1f2e0410";
      flake = false;
    };
    libplacebo-source = {
      url = "git+https://github.com/haasn/libplacebo.git?ref=refs/tags/v7.387.0&rev=275f7dea27924cdd34ab2e6c20e448b6bc4fab65&submodules=1";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, mpv-source, libplacebo-source }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forSystems (system:
        let
          pkg = import ./waguri/nixos/package.nix {
            pkgs = nixpkgs.legacyPackages.${system};
            source = mpv-source;
            upstreamRevision = mpv-source.rev;
            libplaceboSource = libplacebo-source;
            overlaySource = ./waguri;
            guide = ./WAGURI-NIXOS.md;
          };
        in
        {
          default = pkg;
          waguri-mpv = pkg;
        });

      apps = forSystems (system:
        let
          pkg = self.packages.${system}.default;
        in
        {
          default = { type = "app"; program = "${pkg}/bin/waguri-mpv"; };
          mpv = { type = "app"; program = "${pkg}/bin/mpv"; };
        });

      devShells = forSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.default ];
            packages = [ pkgs.gitMinimal ];
          };
        });
    };
}
