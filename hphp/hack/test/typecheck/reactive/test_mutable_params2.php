<?hh // strict
<<file: __EnableUnstableFeatures('coeffects_provisional')>>

class Test {

  public function __construct(public int $val) {}
}

<<__Rx>>
function foo(<<__Mutable>>Test $x, Test $y): void {
  $x->val = 5;
  // error, $x is borrowed
  $y = \HH\Rx\freeze($x);
}
