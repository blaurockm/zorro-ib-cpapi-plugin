# Interactive Brokers Client Portal API Plugin for Zorro

## Using the plugin

1. Download the client portal gateway. (+ Java if necessary)
2. Put plugin-dll into Zorro-plugin folder.

### Authentication

Login into client portal on https://localhost:5000/. 
Accept insecure connection or provide a valid certificate. 
Username & password in Zorro are not used. 
Interactive Brokers requires an authentication once a day. 
Automatic (re-)authentication with reduced security of your account can be achieved with https://github.com/Voyz/ibeam

### Trading limitations

you have one session for all active Zorro-Instances. 
That means: all your scripts shared the limitation of 10 market-data-requests per second. All scripts running with this plugin cannot 
have more than 5 assets with minute-bar-data in total.

#### pip-size & pip-cost

The IB-API provides the fields for this information, albeit they are not always filled correctly.
At the moment Sock-Assets have sizeIncrement (**LotAmount**) of 100. Also when used as CFDs.
So, until the IB guys fix this problem, you will have to enter the pip-infos manually in the assets list.


#### Forex-Trading

At the moment (7.1.2024) not all currency-pairs are supported. For example USD/EUR is missing, EUR/USD is available.

Note: Symbols for forex trading *must* have the form "targetCurrency-CASH--counterCurrency" ("USD-CASH--EUR" for USD/EUR).
The exchange maybe filled, if not the IB standard will be used, which is IDEALPRO


#### Ordertypes

Default order type depends on the asset. Usually "DAY", meaning valid till closing of market.
If you want the plugin to report immediately if the order was executed, set order_type with **brokerCommand()** 
to ORDERTYPE_AON.


## Compiling the plugin

for now you have to use Visual Studio, community edition is ok.
You have to have a Zorro installation for the header files and ZorroDLL.cpp

you need vcpkg - manager: json-c package will be statically linked to the plugin.

"vcpkg install json-c:x86-windows-static"


## Links

* [Zorro Project](www.zorro-project.org)
* [IB-Plugin](https://www.interactivebrokers.com/en/trading/ib-api.php)
* [IB-Campus for API](https://ibkrcampus.com/ibkr-api-page/cpapi/)

