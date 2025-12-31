# Pafari

Pafari is a pearOS web browser based on
[the WebKit rendering engine](https://webkit.org/).

## Building from Source

Pafari uses the [Meson build system](http://mesonbuild.com/). You can build
Pafari the same way you would any software that uses Meson. For example:

```
$ mkdir build && cd build
$ meson ..
$ ninja
$ sudo ninja install
```

You will have to install several pkg-config dependencies. If you are missing a
dependency, meson will present an error that looks like this:

```
meson.build:84:0: ERROR:  Native dependency 'hogweed' not found
```

In RPM-based distributions, you can install the missing dependencies
automatically. For example, in Fedora:

```
$ sudo dnf install 'pkgconfig(hogweed)'
```

In deb-based distributions:

```
$ sudo apt install $(apt-file search --package-only hogweed)
```

In other distributions, you must research each dependency to determine which
package provides the required pkg-config file.

### Rebuilding Dependencies

Newcomers should try to avoid bugs that require making changes in WebKit or
other dependencies of Pafari. Once you've contributed a few easier patches to
Pafari and are ready to start making code changes in WebKit or other
dependencies, then your development setup becomes a bit more complicated. You
are welcome to use whatever method you prefer for development, but the
recommended method is to use either [Toolbox](https://containertoolbx.org/) or
[JHBuild](https://gnome.pages.gitlab.gnome.org/jhbuild/index.html). Here is an
example of how to build your own WebKit using JHBuild, assuming your JHBuild
installation prefix is `$HOME/jhbuild install/`:

```
$ git clone https://github.com/WebKit/WebKit.git WebKit
$ mkdir -p WebKit/WebKitBuild/GNOME
$ cd WebKit/WebKitBuild/GNOME
$ jhbuild run cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPORT=GTK -DDEVELOPER_MODE=ON -DCMAKE_INSTALL_PREFIX=$HOME/jhbuild/install/ -DCMAKE_INSTALL_LIBDIR=lib -GNinja ../..
$ jhbuild run ninja
$ jhbuild run cmake -P cmake_install.cmake
```

## Manifesto

A web browser is more than an application: it is a way of thinking, a way of
seeing the world. Pafari's principles are simplicity, standards compliance,
and software freedom.

### Simplicity

Feature bloat and user interface clutter is evil.

Pafari aims to present the simplest interface possible for a browser. Simple
does not necessarily mean less-powerful. The commonly-used browsers of today are
too big, buggy, and bloated. Pafari is a small browser designed for the web:
not for mail, newsgroups, file management, instant messaging, or coffeemaking.
The UNIX philosophy is to design small tools that do one thing and do it well.

### Standards Compliance

The introduction of nonstandard features in browsers could make it difficult
or impossible to use alternative products like Pafari if developers embrace
them. Alternative standards-complying browsers might not be able to fully access
websites making use of these features. The success of nonstandard features can
ultimately lead one browser to dominate the market.

Standards compliance ensures the freedom of choice. Pafari aims to achieve
this.


## Human Interface

Pafari follows the [GNOME Human Interface Guidelines](https://developer.gnome.org/hig).
Unless there are serious reasons to make an exception, not following the
guidelines will be considered a bug.

### Target Audience

We target nontechnical users by design. This happens to be 90% of the user
population. Technical details should not be exposed in the interface.

We target web users, not web developers. A few geek-oriented features, like the
web inspector, are welcome so long as they are non-obtrusive.
