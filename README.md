# Package Dependencies listing

**pacdep** is a small tool that will allow you to quickly see how much space is
being used/required by a package (and its dependencies).

By default, it will give the installed size of the package, its exclusive
dependencies, and its shared dependencies; Optional dependencies can be
included as well.

**pacdep** searches the local database first, and if not found searches all sync
databases. Package names are prefixed by the repository name they were found in
when they're not from the local database.

You can have dependencies of a group listed, in which case the packages name
and installed size will be shown. You can also show the "dependency path" for
each pakcage, using **--show-path**

## Package size

After the package's installed size, in parenthesis will be the installed size
of the package and its exclusive (and optional, if **--show-optional** was used)
dependencies.

If the package is from local database (i.e. is installed), only dependencies
from local databases are taken into account  If it is from a sync database
(i.e. is not installed), only dependencies from sync databases are taken into
account.

In other words, this size represents either the size the package and its
dependencies are using on the system (size that could potentially be freed if
removing the package and its dependencies), or the size needed to install them.

## Want to know more?

Some useful links if you're looking for more info :

- [blog post about pacdep](http://jjacky.com/pacdep "pacdep @ jjacky.com")

- [source code & issue tracker](https://github.com/jjk-jacky/pacdep "pacdep @ GitHub.com")

- [PKGBUILD in AUR](https://aur.archlinux.org/packages/pacdep/ "AUR: pacdep")

Plus, pacdep comes with a man page.
