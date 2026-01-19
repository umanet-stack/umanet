#!/usr/bin/env perl

use File::Find;
use File::Path qw(make_path);

my %data;
my %data_rate;

if (@ARGV != 1) {
    print "Usage: transform.pl <percentile>\n";
    exit 1;
}

print "Aggregating all data for rate $ARGV[0]\n";

find(
    {
        wanted => sub {
            return unless -f && /testing\/wrk\/out\/([\w\d]+)\/([\.\w\d]+)\/(\d+)/;
            my ($name, $R, $ID) = ($1, $2, $3);
            my $out = `awk -f extract.awk $_ $ARGV[0]`;
            $data{$name}{$R}{$ID} = $out;
            $data_rate{$name}{$R}{$ID} = `awk '\$1=="Requests/sec:" { printf "%.2f", \$2 }' $_`;
        },
        no_chdir => 1,
    }, 'testing/wrk/out'
);

for my $name (sort keys %data) {
    for my $R (sort keys %{$data{$name}}) {
        my $base = "testing/wrk/agg/$name/$R/$ARGV[0]";
        make_path($base);
        my $each_path = "$base/each";
        open my $fh, ">", $each_path;
        print $fh "ID,Latency,ActualRate\n";
        for my $ID (sort keys %{$data{$name}{$R}}) {
            print $fh "$ID,$data{$name}{$R}{$ID},$data_rate{$name}{$R}{$ID}\n";
        }
        close $fh;
    }
}
